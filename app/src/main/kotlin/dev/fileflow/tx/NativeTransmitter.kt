package dev.fileflow.tx

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * JNI bridge to the optical frame generator (C04).
 *
 * ADR-0014: the frame content comes from the same portable C++ the desktop suite and simulator
 * exercise, so a frame presented on a phone is the frame `ffsim` would have produced for the same
 * session. That equivalence is what makes a real capture comparable to a simulated one.
 *
 * ⚠ FIELD AND METHOD NAMES ARE PART OF THE ABI — see the note in `NativeProbe.kt`.
 */
class Transmitter private constructor(
    private var handle: Long,
    val cols: Int,
    val rows: Int,
    /** The name the receiver is told — a display hint, which it sanitises. */
    val fileName: String,
) : AutoCloseable {

    /** Size of the file being sent, in bytes. */
    val fileBytes: Long = NativeTransmitter.fileSize(handle)

    /**
     * One byte per cell, row-major — the exact layout the GL texture wants.
     *
     * DIRECT and native-ordered so `nextFrame` writes into it through
     * `GetDirectBufferAddress` with no copy (ADR-0003). Allocated once and reused: this is the
     * per-frame path, and CONTRIBUTING bans allocation there.
     */
    val cells: ByteBuffer =
        ByteBuffer.allocateDirect(cols * rows).order(ByteOrder.nativeOrder())

    companion object {
        init {
            System.loadLibrary("fileflow")
        }

        /**
         * Opens a transmit session for a grid the DEVICE can render at an integer cell pitch.
         *
         * The grid is device-dependent and must be chosen from the probe's integer-pitch list, not
         * assumed: a fractional pitch puts cell boundaries on fractional pixels, which the panel
         * cannot render crisply and which raises spatial crosstalk for no gain (DEVICE-MATRIX).
         */
        fun open(
            cols: Int,
            rows: Int,
            nsym: Int = 32,
            payloadBytes: Int = 128 * 1024,
            seed: Long = 5,
            name: String = TEST_FILE_NAME,
        ): Transmitter = Transmitter(
            NativeTransmitter.open(
                cols, rows, nsym, payloadBytes, seed, name.toByteArray(Charsets.UTF_8),
            ),
            cols, rows, name,
        )

        /** Sends a real file: its bytes, and the name the receiver will save it under. */
        fun openFile(cols: Int, rows: Int, nsym: Int, data: ByteArray, name: String): Transmitter =
            Transmitter(
                NativeTransmitter.openFile(cols, rows, nsym, data, name.toByteArray(Charsets.UTF_8)),
                cols, rows, name,
            )

        /** What the receiver saves the generated test file as. */
        const val TEST_FILE_NAME = "fileflow-test.bin"
    }

    /** File bytes carried by each data frame — what a transfer-time estimate is built on. */
    val symbolBytes: Int
        get() = if (handle != 0L) NativeTransmitter.symbolBytes(handle) else -1

    /**
     * Renders the next display state into [cells].
     *
     * Returns 0 on success or a `fileflow::Error` ordinal. A failure is a **data point, not an
     * exception** — it happens on the render thread, where throwing per frame would be both slow
     * and wrong.
     */
    fun nextFrame(): Int {
        val h = handle
        if (h == 0L) return -1
        return NativeTransmitter.nextFrame(h, cells)
    }

    /** Sequence number of the state most recently rendered. */
    val lastSequence: Int
        get() = if (handle != 0L) NativeTransmitter.lastSequence(handle) else -1

    /**
     * SHA-256 of the payload being transmitted.
     *
     * Goes into the capture metadata so a receiver's claim to have recovered the file can be
     * checked. Without it a completed transfer proves the bytes arrived, not that the *right* bytes
     * arrived — different claims, and G3 is about the second one.
     */
    val payloadSha256: String
        get() = if (handle != 0L) NativeTransmitter.payloadSha256(handle) else ""

    /** Payload cells per frame — the numerator of the raw optical bit rate at M0's 1 bit/cell. */
    val payloadCells: Int
        get() = if (handle != 0L) NativeTransmitter.payloadCells(handle) else -1

    override fun close() {
        val h = handle
        handle = 0L
        if (h != 0L) NativeTransmitter.close(h)
    }
}

/** Raw external declarations. Use [Transmitter] rather than calling these directly. */
internal object NativeTransmitter {
    @JvmStatic external fun open(
        cols: Int, rows: Int, nsym: Int, payloadBytes: Int, seed: Long, nameUtf8: ByteArray,
    ): Long
    @JvmStatic external fun openFile(
        cols: Int, rows: Int, nsym: Int, data: ByteArray, nameUtf8: ByteArray,
    ): Long
    @JvmStatic external fun symbolBytes(handle: Long): Int
    @JvmStatic external fun fileSize(handle: Long): Long
    @JvmStatic external fun nextFrame(handle: Long, dst: ByteBuffer): Int
    @JvmStatic external fun lastSequence(handle: Long): Int
    @JvmStatic external fun payloadSha256(handle: Long): String
    @JvmStatic external fun payloadCells(handle: Long): Int
    @JvmStatic external fun close(handle: Long)
}
