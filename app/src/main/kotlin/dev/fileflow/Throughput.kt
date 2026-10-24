package dev.fileflow

/**
 * What compression did to a transfer's speed, counting its cost as well as its saving.
 *
 * The link moves [linkBytes] -- the file as compressed -- at [linkRate] bytes a second; the person
 * waits for [originalBytes]. Compressing costs the sender [compressMs] before the first frame, and
 * opening costs the receiver [decompressMs] after the last, and both count: compression that saves
 * less link time than it costs is a loss, and is reported as one.
 */
data class Throughput(
    val originalBytes: Long,
    val linkBytes: Long,
    val linkRate: Double,
    val compressMs: Long = 0,
    val decompressMs: Long = 0,
) {
    val valid: Boolean get() = originalBytes > 0 && linkBytes > 0 && linkRate > 0.0

    /** How many times smaller the file crossed the link. */
    val ratio: Double get() = if (linkBytes > 0) originalBytes.toDouble() / linkBytes else 1.0

    /** Seconds in all: on the link, plus compressing and opening. */
    val totalSeconds: Double
        get() = if (valid) linkBytes / linkRate + (compressMs + decompressMs) / 1000.0 else 0.0

    /** The file's own bytes per second, every cost counted. */
    val effectiveRate: Double get() = if (valid) originalBytes / totalSeconds else 0.0

    /** What sending it uncompressed would have taken over the same link. */
    val uncompressedSeconds: Double get() = if (valid) originalBytes / linkRate else 0.0

    /** Positive: seconds compression saved; negative: seconds it cost. */
    val secondsSaved: Double get() = uncompressedSeconds - totalSeconds

    companion object {
        /** "zstd", "deflate", or null for none -- the envelope's codec ids. */
        fun codecName(codec: Int): String? = when (codec) {
            1 -> "Deflate"
            2 -> "zstd"
            else -> null
        }

        /** "saves 9 s" / "costs 0.4 s": the sign said in words, at a precision that holds still. */
        fun describeSaving(seconds: Double): String {
            val s = Math.abs(seconds)
            val n = if (s >= 10) "%.0f".format(s) else "%.1f".format(s)
            return if (seconds >= 0) "saves $n s" else "costs $n s"
        }
    }
}
