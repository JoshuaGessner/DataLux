package dev.fileflow.secure

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.security.SecureRandom
import java.util.zip.Deflater
import java.util.zip.Inflater
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec

/**
 * What actually travels over the light link: the file, compressed when that makes it smaller,
 * and sealed to a paired phone when encrypted mode is on.
 *
 * ```
 * "FFE1" | flags | [key id (8) | nonce (12)]  | body
 *          bit 0 compressed, bit 1 encrypted     ^ only when encrypted
 * body  = AES-256-GCM(inner), header bytes as associated data   (encrypted)
 *       | inner                                                  (otherwise)
 * inner = name length (u16) | name (UTF-8) | original size (u64) | data, raw-deflated if compressed
 * ```
 *
 * Compression comes before encryption, because ciphertext does not compress. The file's NAME is
 * inside the envelope, so an encrypted transfer shows nothing but its size to a camera that is not
 * the paired one. Every transfer is already verified end to end by the link's SHA-256; under
 * encryption the GCM tag additionally proves the bytes came from the paired phone, unaltered.
 */
object Envelope {
    private val MAGIC = byteArrayOf('F'.code.toByte(), 'F'.code.toByte(), 'E'.code.toByte(), '1'.code.toByte())
    private const val FLAG_COMPRESSED = 1
    private const val FLAG_ENCRYPTED = 2
    private const val NONCE_BYTES = 12
    private const val TAG_BITS = 128
    private const val MAX_NAME_BYTES = 255

    /** Largest file an envelope may claim to unpack to: a bound on any decompression bomb. */
    const val MAX_ORIGINAL_BYTES = 512L * 1024 * 1024

    /** Below this, compression cannot pay for its own header. */
    private const val MIN_COMPRESS_BYTES = 512

    /** How much of a file the compressibility probe deflates. */
    private const val PROBE_BYTES = 256 * 1024

    /**
     * A probe that saves less than this means the file is already compressed -- a photo, a video,
     * an archive -- and deflating all of it would cost time for nothing.
     */
    private const val PROBE_MAX_RATIO = 0.92

    /** The whole result must save at least this much, or the original is sent. */
    private const val KEEP_MAX_RATIO = 0.97

    class Packed(
        val bytes: ByteArray,
        val originalSize: Long,
        val compressed: Boolean,
        val encrypted: Boolean,
    )

    class Unpacked(
        val name: String,
        val data: ByteArray,
        val compressed: Boolean,
        val encrypted: Boolean,
        /** Bytes that crossed the link, for the effective-speed figure. */
        val packedSize: Int,
    )

    enum class Failure(val message: String) {
        NotAnEnvelope("This isn't a FileFlow file."),
        NeedsPairing("This file is encrypted. Pair the two phones, then send it again."),
        WrongPairing("This file is encrypted for a different pairing, or was altered. Nothing was saved."),
        NotEncrypted("The sender isn't in encrypted mode, so the file was refused. Turn encrypted mode on " +
            "on both phones, or off on this one."),
        Corrupt("The file is damaged. Nothing was saved."),
    }

    class UnpackException(val failure: Failure) : Exception(failure.message)

    /**
     * Wraps [data] for sending: compressed when that saves more than a few percent, encrypted to
     * [key] when given.
     */
    fun pack(data: ByteArray, name: String, key: PairingKey?, random: SecureRandom = SecureRandom()): Packed {
        require(data.size.toLong() <= MAX_ORIGINAL_BYTES) { "file too large" }
        val body = compressIfWorthIt(data)
        val compressed = body !== data
        val nameBytes = name.toByteArray(Charsets.UTF_8).let {
            if (it.size <= MAX_NAME_BYTES) it else it.copyOf(MAX_NAME_BYTES)
        }
        val inner = ByteBuffer.allocate(2 + nameBytes.size + 8 + body.size)
            .putShort(nameBytes.size.toShort())
            .put(nameBytes)
            .putLong(data.size.toLong())
            .put(body)
            .array()

        var flags = if (compressed) FLAG_COMPRESSED else 0
        if (key == null) {
            val out = ByteArray(MAGIC.size + 1 + inner.size)
            MAGIC.copyInto(out)
            out[MAGIC.size] = flags.toByte()
            inner.copyInto(out, MAGIC.size + 1)
            return Packed(out, data.size.toLong(), compressed, encrypted = false)
        }

        flags = flags or FLAG_ENCRYPTED
        val nonce = ByteArray(NONCE_BYTES).also(random::nextBytes)
        val header = ByteBuffer.allocate(MAGIC.size + 1 + PairingKey.ID_BYTES + NONCE_BYTES)
            .put(MAGIC).put(flags.toByte()).put(key.id).put(nonce).array()
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key.secret, GCMParameterSpec(TAG_BITS, nonce))
        cipher.updateAAD(header)
        val sealed = cipher.doFinal(inner)
        return Packed(header + sealed, data.size.toLong(), compressed, encrypted = true)
    }

    /**
     * Opens what [pack] made. [key] is the current pairing, if any; with [requireEncrypted] an
     * envelope that is not encrypted is refused, so encrypted mode cannot be talked down to plain.
     */
    fun unpack(bytes: ByteArray, key: PairingKey?, requireEncrypted: Boolean): Unpacked {
        if (!isEnvelope(bytes) || bytes.size < MAGIC.size + 1) throw UnpackException(Failure.NotAnEnvelope)
        val flags = bytes[MAGIC.size].toInt() and 0xFF
        if (flags and (FLAG_COMPRESSED or FLAG_ENCRYPTED).inv() != 0) throw UnpackException(Failure.Corrupt)
        val encrypted = flags and FLAG_ENCRYPTED != 0
        if (requireEncrypted && !encrypted) throw UnpackException(Failure.NotEncrypted)

        val inner: ByteArray = if (encrypted) {
            val headerSize = MAGIC.size + 1 + PairingKey.ID_BYTES + NONCE_BYTES
            if (bytes.size < headerSize + TAG_BITS / 8) throw UnpackException(Failure.Corrupt)
            if (key == null) throw UnpackException(Failure.NeedsPairing)
            val id = bytes.copyOfRange(MAGIC.size + 1, MAGIC.size + 1 + PairingKey.ID_BYTES)
            if (!java.security.MessageDigest.isEqual(id, key.id)) throw UnpackException(Failure.WrongPairing)
            val nonce = bytes.copyOfRange(MAGIC.size + 1 + PairingKey.ID_BYTES, headerSize)
            try {
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                cipher.init(Cipher.DECRYPT_MODE, key.secret, GCMParameterSpec(TAG_BITS, nonce))
                cipher.updateAAD(bytes, 0, headerSize)
                cipher.doFinal(bytes, headerSize, bytes.size - headerSize)
            } catch (e: javax.crypto.AEADBadTagException) {
                throw UnpackException(Failure.WrongPairing)
            }
        } else {
            bytes.copyOfRange(MAGIC.size + 1, bytes.size)
        }

        val buf = ByteBuffer.wrap(inner)
        if (buf.remaining() < 2) throw UnpackException(Failure.Corrupt)
        val nameLen = buf.short.toInt() and 0xFFFF
        if (nameLen > MAX_NAME_BYTES || buf.remaining() < nameLen + 8) throw UnpackException(Failure.Corrupt)
        val name = ByteArray(nameLen).also { buf.get(it) }.toString(Charsets.UTF_8)
        val original = buf.long
        if (original < 0 || original > MAX_ORIGINAL_BYTES) throw UnpackException(Failure.Corrupt)
        val body = ByteArray(buf.remaining()).also { buf.get(it) }
        val compressed = flags and FLAG_COMPRESSED != 0
        val data = if (compressed) inflate(body, original.toInt()) else body
        if (data.size.toLong() != original) throw UnpackException(Failure.Corrupt)
        return Unpacked(name, data, compressed, encrypted, bytes.size)
    }

    fun isEnvelope(bytes: ByteArray): Boolean =
        bytes.size >= MAGIC.size && MAGIC.indices.all { bytes[it] == MAGIC[it] }

    /**
     * "Dynamic" compression: decided per file, from a sample of the file itself. A quick deflate of
     * the first 256 KB says whether the file is already compressed; if it is, the whole file is not
     * touched, so a photo costs nothing. If it is not, the whole file is deflated -- harder for a
     * small file, faster for a big one -- and kept only if it really is smaller.
     */
    private fun compressIfWorthIt(data: ByteArray): ByteArray {
        if (data.size < MIN_COMPRESS_BYTES) return data
        val probe = deflate(data, 0, minOf(data.size, PROBE_BYTES), Deflater.BEST_SPEED)
        if (probe.size > minOf(data.size, PROBE_BYTES) * PROBE_MAX_RATIO) return data
        val level = if (data.size <= 8 * 1024 * 1024) Deflater.BEST_COMPRESSION else 6
        val whole = deflate(data, 0, data.size, level)
        return if (whole.size < data.size * KEEP_MAX_RATIO) whole else data
    }

    private fun deflate(data: ByteArray, offset: Int, length: Int, level: Int): ByteArray {
        val d = Deflater(level, true)
        try {
            d.setInput(data, offset, length)
            d.finish()
            val out = ByteArrayOutputStream(length / 2 + 64)
            val chunk = ByteArray(64 * 1024)
            while (!d.finished()) out.write(chunk, 0, d.deflate(chunk))
            return out.toByteArray()
        } finally {
            d.end()
        }
    }

    /** Inflates to EXACTLY [expected] bytes, never more: the size is a bound, not a hint. */
    private fun inflate(body: ByteArray, expected: Int): ByteArray {
        val inf = Inflater(true)
        try {
            inf.setInput(body)
            val out = ByteArray(expected)
            var n = 0
            while (n < expected) {
                val got = inf.inflate(out, n, expected - n)
                if (got == 0) {
                    if (inf.finished() || inf.needsInput() || inf.needsDictionary()) break
                }
                n += got
            }
            // More data than declared is a lie about the size, and the cheapest form of bomb.
            if (n != expected || !inf.finished() && inf.inflate(ByteArray(1)) > 0) {
                throw UnpackException(Failure.Corrupt)
            }
            return out
        } catch (e: java.util.zip.DataFormatException) {
            throw UnpackException(Failure.Corrupt)
        } finally {
            inf.end()
        }
    }
}
