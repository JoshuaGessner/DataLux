package dev.fileflow.secure

/**
 * How a payload's bytes are compressed. An interface so the envelope can be unit-tested on a
 * desktop JVM, where the native library -- and so zstd -- is not loaded.
 */
interface Codec {
    /** The id an envelope records: 1 deflate, 2 zstd. */
    val id: Int

    /** The strongest level, and the one [compress] uses for a quick probe. */
    val maxLevel: Int
    val probeLevel: Int

    /** Null when it could not. */
    fun compress(data: ByteArray, level: Int, longWindow: Boolean): ByteArray?

    /** Exactly [expected] bytes or null: the size is a bound, never a hint. */
    fun decompress(data: ByteArray, offset: Int, length: Int, expected: Int): ByteArray?
}

/**
 * Zstandard, through the native library (core/compress.h). Long-distance matching over a 128 MB
 * window, so repeats anywhere in a big file count -- Deflate sees only 32 KB back.
 */
object ZstdCodec : Codec {
    override val id = 2
    override val maxLevel: Int by lazy { runCatching { NativeCodec.zstdMaxLevel() }.getOrDefault(19) }
    override val probeLevel = 1

    /** False off-device (unit tests), where the library is not there to load. */
    val available: Boolean by lazy {
        runCatching {
            System.loadLibrary("fileflow")
            NativeCodec.zstdMaxLevel() > 0
        }.getOrDefault(false)
    }

    override fun compress(data: ByteArray, level: Int, longWindow: Boolean): ByteArray? =
        NativeCodec.zstdCompress(data, level, longWindow)

    override fun decompress(data: ByteArray, offset: Int, length: Int, expected: Int): ByteArray? =
        NativeCodec.zstdDecompress(data, offset, length, expected)
}

/** Raw Deflate from java.util.zip: the fallback, and the codec of version-1 envelopes. */
object DeflateCodec : Codec {
    override val id = 1
    override val maxLevel = java.util.zip.Deflater.BEST_COMPRESSION
    override val probeLevel = java.util.zip.Deflater.BEST_SPEED

    override fun compress(data: ByteArray, level: Int, longWindow: Boolean): ByteArray? {
        val d = java.util.zip.Deflater(level.coerceIn(1, maxLevel), true)
        try {
            d.setInput(data)
            d.finish()
            val out = java.io.ByteArrayOutputStream(data.size / 2 + 64)
            val chunk = ByteArray(64 * 1024)
            while (!d.finished()) out.write(chunk, 0, d.deflate(chunk))
            return out.toByteArray()
        } finally {
            d.end()
        }
    }

    override fun decompress(data: ByteArray, offset: Int, length: Int, expected: Int): ByteArray? {
        val inf = java.util.zip.Inflater(true)
        try {
            inf.setInput(data, offset, length)
            val out = ByteArray(expected)
            var n = 0
            while (n < expected) {
                val got = inf.inflate(out, n, expected - n)
                if (got == 0 && (inf.finished() || inf.needsInput() || inf.needsDictionary())) break
                n += got
            }
            // More data than declared is a lie about the size, and the cheapest form of bomb.
            if (n != expected || !inf.finished() && inf.inflate(ByteArray(1)) > 0) return null
            return out
        } catch (e: java.util.zip.DataFormatException) {
            return null
        } finally {
            inf.end()
        }
    }
}

/** Raw JNI declarations; use [ZstdCodec]. Null returns are failures. */
internal object NativeCodec {
    @JvmStatic external fun zstdCompress(data: ByteArray, level: Int, longWindow: Boolean): ByteArray?
    @JvmStatic external fun zstdDecompress(data: ByteArray, offset: Int, length: Int, expected: Int): ByteArray?
    @JvmStatic external fun zstdMaxLevel(): Int
}
