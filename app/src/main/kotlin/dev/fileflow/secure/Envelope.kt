package dev.fileflow.secure

import java.nio.ByteBuffer
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * What actually travels over the light link: the file, compressed when that makes it smaller,
 * and sealed to a paired phone when encrypted mode is on.
 *
 * ```
 * "FFE2" | flags | [key id (8) | nonce (12)]  | body
 *          bit 1 encrypted                        ^ only when encrypted
 *          bit 2 a batch: the data is several files (below)
 * body  = AES-256-GCM(inner), header bytes as associated data   (encrypted)
 *       | inner                                                  (otherwise)
 * inner = name length (u16) | name (UTF-8) | original size (u64) | codec (u8) | compress ms (u32)
 *         | data, compressed by the codec (0 none, 1 deflate, 2 zstd)
 * ```
 *
 * A batch is one transfer of several files -- the link is one-way, so the sender cannot know when
 * the receiver has finished one file and move on to the next; sending them as one means one aim,
 * one progress bar, and compression across all of them:
 *
 * ```
 * data  = "FFB1" | count (u16) | count x [ name length (u16) | name (UTF-8) | size (u64) | bytes ]
 * ```
 *
 * Version 1 ("FFE1", deflate only, flag bit 0 = compressed) is still read.
 *
 * The key id names the key an envelope is sealed to -- a pairing's, or a team's ([Team]) -- so a
 * receiver holding several opens it with the right one, and says plainly when it holds none.
 *
 * Compression comes before encryption, because ciphertext does not compress. The file's NAME is
 * inside the envelope, so an encrypted transfer shows nothing but its size to a camera that is not
 * the paired one. Every transfer is already verified end to end by the link's SHA-256; under
 * encryption the GCM tag additionally proves the bytes came from the paired phone, unaltered.
 */
/** A key an envelope can be sealed to: a pairing's ([PairingKey]) or a team's ([Team]). */
interface SealKey {
    val secret: SecretKeySpec
    /** [PairingKey.ID_BYTES] bytes, written in the clear in each envelope sealed to this key. */
    val id: ByteArray
    /** For the person: "paired phone", "team Field crew". */
    val label: String
}

object Envelope {
    private val MAGIC_V1 = "FFE1".toByteArray(Charsets.US_ASCII)
    private val MAGIC = "FFE2".toByteArray(Charsets.US_ASCII)
    private const val FLAG_V1_COMPRESSED = 1
    private const val FLAG_ENCRYPTED = 2
    private const val FLAG_BATCH = 4
    private val MAGIC_BATCH = "FFB1".toByteArray(Charsets.US_ASCII)

    /** Most files one batch may carry. */
    const val MAX_BATCH_FILES = 1000
    private const val NONCE_BYTES = 12
    private const val TAG_BITS = 128
    private const val MAX_NAME_BYTES = 255

    const val CODEC_NONE = 0

    /** Largest file an envelope may claim to unpack to: a bound on any decompression bomb. */
    const val MAX_ORIGINAL_BYTES = 512L * 1024 * 1024

    /** Below this, compression cannot pay for its own header. */
    private const val MIN_COMPRESS_BYTES = 512

    /** How much of a file the compressibility probe compresses. */
    private const val PROBE_BYTES = 256 * 1024

    /**
     * A probe that saves less than this means the file is already compressed -- a photo, a video,
     * an archive -- and compressing all of it would cost time for nothing.
     */
    private const val PROBE_MAX_RATIO = 0.92

    /** The whole result must save at least this much, or the original is sent. */
    private const val KEEP_MAX_RATIO = 0.97

    /**
     * How hard to compress.
     *
     * The light link moves a few hundred kilobytes a second, so compressing is nearly always
     * worth its time: zstd's strong levels compress a few megabytes a second on a phone, and each
     * byte they save is a byte that need not cross the link at all.
     */
    enum class Mode {
        /** Never: the file is sent as it is. */
        Off,
        /**
         * Only what compresses -- a quick probe of the start of the file decides -- at a strong
         * level, with a long match window once the file is big enough for one to matter.
         */
        Auto,
        /** The strongest level and the longest window, for every file that compresses at all. */
        Maximum,
    }

    class Packed(
        val bytes: ByteArray,
        val originalSize: Long,
        /** 0 none, 1 deflate, 2 zstd. */
        val codec: Int,
        /** What compressing (and probing) cost, ms. */
        val compressMs: Long,
        val encrypted: Boolean,
    ) {
        val compressed: Boolean get() = codec != CODEC_NONE
    }

    class Unpacked(
        val name: String,
        val data: ByteArray,
        val codec: Int,
        val encrypted: Boolean,
        /** Bytes that crossed the link, for the effective-speed figure. */
        val packedSize: Int,
        /** What the SENDER spent compressing (or deciding not to), from the envelope. */
        val compressMs: Long,
        /** What opening it cost here, ms. */
        val decompressMs: Long,
        /** A batch's files, name and bytes, in the order sent; null for a single file. */
        val files: List<Pair<String, ByteArray>>? = null,
        /** The key that opened it, when it was sealed. */
        val sealedTo: SealKey? = null,
    ) {
        val compressed: Boolean get() = codec != CODEC_NONE
    }

    enum class Failure(val message: String) {
        NotAnEnvelope("This isn't a DataLux file."),
        NeedsPairing("This file is encrypted. Pair the two phones, or join the sender's team, then send it again."),
        WrongPairing("This file is encrypted for a pairing or team this phone isn't in, or was altered. " +
            "Nothing was saved."),
        NotEncrypted("The sender isn't in encrypted mode, so the file was refused. Turn encrypted mode on " +
            "on both phones, or off on this one."),
        UnknownCodec("This file was compressed in a way this version of DataLux can't open. Update it."),
        Corrupt("The file is damaged. Nothing was saved."),
        NeedsPro("This batch needs DataLux Pro on the receiving phone. Nothing was saved. " +
            "Upgrade or restore Pro here, or ask the sender to send one file at a time for free."),
    }

    class UnpackException(val failure: Failure) : Exception(failure.message)

    /** The codec this build compresses with: zstd where the native library is, else Deflate. */
    val defaultCodec: Codec get() = if (ZstdCodec.available) ZstdCodec else DeflateCodec

    private fun codecFor(id: Int): Codec? = when (id) {
        ZstdCodec.id -> if (ZstdCodec.available) ZstdCodec else null
        DeflateCodec.id -> DeflateCodec
        else -> null
    }

    /**
     * Wraps [data] for sending: compressed as [mode] says, when that saves more than a few
     * percent; encrypted to [key] when given.
     */
    fun pack(
        data: ByteArray, name: String, key: SealKey?,
        mode: Mode = Mode.Auto, codec: Codec = defaultCodec, random: SecureRandom = SecureRandom(),
    ): Packed = pack(data, name, key, mode, codec, random, batch = false)

    /**
     * Wraps several files as ONE transfer (see the format above), named for the person by
     * [name]. Compressed and encrypted as a whole, like a single file.
     */
    fun packBatch(
        files: List<Pair<String, ByteArray>>, name: String, key: SealKey?,
        mode: Mode = Mode.Auto, codec: Codec = defaultCodec, random: SecureRandom = SecureRandom(),
    ): Packed {
        require(files.isNotEmpty() && files.size <= MAX_BATCH_FILES) { "1..$MAX_BATCH_FILES files" }
        val names = files.map { (n, _) -> truncatedName(n) }
        val size = MAGIC_BATCH.size + 2 + files.indices.sumOf { 2L + names[it].size + 8 + files[it].second.size }
        require(size <= MAX_ORIGINAL_BYTES) { "batch too large" }
        val buf = ByteBuffer.allocate(size.toInt()).put(MAGIC_BATCH).putShort(files.size.toShort())
        files.forEachIndexed { i, (_, bytes) ->
            buf.putShort(names[i].size.toShort()).put(names[i]).putLong(bytes.size.toLong()).put(bytes)
        }
        return pack(buf.array(), name, key, mode, codec, random, batch = true)
    }

    private fun truncatedName(name: String): ByteArray = name.toByteArray(Charsets.UTF_8).let {
        if (it.size <= MAX_NAME_BYTES) it else it.copyOf(MAX_NAME_BYTES)
    }

    private fun pack(
        data: ByteArray, name: String, key: SealKey?,
        mode: Mode, codec: Codec, random: SecureRandom, batch: Boolean,
    ): Packed {
        require(data.size.toLong() <= MAX_ORIGINAL_BYTES) { "file too large" }
        val started = System.nanoTime()
        val body = compressIfWorthIt(data, mode, codec)
        val compressMs = if (mode == Mode.Off) 0 else (System.nanoTime() - started) / 1_000_000
        val codecId = if (body !== data) codec.id else CODEC_NONE
        val nameBytes = truncatedName(name)
        val inner = ByteBuffer.allocate(2 + nameBytes.size + 8 + 1 + 4 + body.size)
            .putShort(nameBytes.size.toShort())
            .put(nameBytes)
            .putLong(data.size.toLong())
            .put(codecId.toByte())
            // Recorded even when the probe said no: that cost is part of the transfer too.
            .putInt(compressMs.coerceAtMost(Int.MAX_VALUE.toLong()).toInt())
            .put(body)
            .array()

        if (key == null) {
            val out = ByteArray(MAGIC.size + 1 + inner.size)
            MAGIC.copyInto(out)
            out[MAGIC.size] = (if (batch) FLAG_BATCH else 0).toByte()
            inner.copyInto(out, MAGIC.size + 1)
            return Packed(out, data.size.toLong(), codecId, compressMs, encrypted = false)
        }

        val nonce = ByteArray(NONCE_BYTES).also(random::nextBytes)
        val header = ByteBuffer.allocate(MAGIC.size + 1 + PairingKey.ID_BYTES + NONCE_BYTES)
            .put(MAGIC).put((FLAG_ENCRYPTED or (if (batch) FLAG_BATCH else 0)).toByte())
            .put(key.id).put(nonce).array()
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key.secret, GCMParameterSpec(TAG_BITS, nonce))
        cipher.updateAAD(header)
        val sealed = cipher.doFinal(inner)
        return Packed(header + sealed, data.size.toLong(), codecId, compressMs, encrypted = true)
    }

    /** [unpack] with at most one key. */
    fun unpack(bytes: ByteArray, key: SealKey?, requireEncrypted: Boolean): Unpacked =
        unpack(bytes, listOfNotNull(key), requireEncrypted)

    /**
     * Opens what [pack] made. [keys] are every key this phone holds -- the current pairing, its
     * teams -- and the envelope's key id picks one. With [requireEncrypted] an envelope that is not
     * encrypted is refused, so encrypted mode cannot be talked down to plain.
     */
    fun unpack(bytes: ByteArray, keys: List<SealKey>, requireEncrypted: Boolean, allowBatch: Boolean = true): Unpacked {
        val v1 = hasMagic(bytes, MAGIC_V1)
        if (!v1 && !hasMagic(bytes, MAGIC) || bytes.size < MAGIC.size + 1) {
            throw UnpackException(Failure.NotAnEnvelope)
        }
        val flags = bytes[MAGIC.size].toInt() and 0xFF
        val known = FLAG_ENCRYPTED or (if (v1) FLAG_V1_COMPRESSED else FLAG_BATCH)
        if (flags and known.inv() != 0) throw UnpackException(Failure.Corrupt)
        val encrypted = flags and FLAG_ENCRYPTED != 0
        if (requireEncrypted && !encrypted) throw UnpackException(Failure.NotEncrypted)

        var sealedTo: SealKey? = null
        val inner: ByteArray = if (encrypted) {
            val headerSize = MAGIC.size + 1 + PairingKey.ID_BYTES + NONCE_BYTES
            if (bytes.size < headerSize + TAG_BITS / 8) throw UnpackException(Failure.Corrupt)
            if (keys.isEmpty()) throw UnpackException(Failure.NeedsPairing)
            val id = bytes.copyOfRange(MAGIC.size + 1, MAGIC.size + 1 + PairingKey.ID_BYTES)
            val key = keys.firstOrNull { java.security.MessageDigest.isEqual(id, it.id) }
                ?: throw UnpackException(Failure.WrongPairing)
            sealedTo = key
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
        val codecId: Int
        val compressMs: Long
        if (v1) {
            codecId = if (flags and FLAG_V1_COMPRESSED != 0) DeflateCodec.id else CODEC_NONE
            compressMs = 0
        } else {
            if (buf.remaining() < 5) throw UnpackException(Failure.Corrupt)
            codecId = buf.get().toInt() and 0xFF
            compressMs = buf.int.toLong() and 0xFFFFFFFFL
        }
        val offset = buf.position()
        val started = System.nanoTime()
        val data = if (codecId == CODEC_NONE) {
            inner.copyOfRange(offset, inner.size)
        } else {
            val codec = codecFor(codecId) ?: throw UnpackException(Failure.UnknownCodec)
            codec.decompress(inner, offset, inner.size - offset, original.toInt())
                ?: throw UnpackException(Failure.Corrupt)
        }
        if (data.size.toLong() != original) throw UnpackException(Failure.Corrupt)
        val decompressMs = (System.nanoTime() - started) / 1_000_000
        val files = if (flags and FLAG_BATCH != 0 && !v1) parseBatch(data, allowBatch) else null
        return Unpacked(name, data, codecId, encrypted, bytes.size, compressMs, decompressMs, files, sealedTo)
    }

    /**
     * A batch's files. Every count, length and size is checked against what is actually there:
     * the bytes came off a screen anyone could have been showing (THREAT-MODEL T3).
     */
    private fun parseBatch(data: ByteArray, allowBatch: Boolean): List<Pair<String, ByteArray>> {
        fun corrupt(): Nothing = throw UnpackException(Failure.Corrupt)
        if (data.size < MAGIC_BATCH.size + 2 || !hasMagic(data, MAGIC_BATCH)) corrupt()
        val buf = ByteBuffer.wrap(data)
        buf.position(MAGIC_BATCH.size)
        val count = buf.short.toInt() and 0xFFFF
        if (count == 0 || count > MAX_BATCH_FILES) corrupt()
        // Validate the complete structure without allocating members before offering an upgrade.
        // Corrupt input must not become a purchase prompt that fails after checkout.
        val membersAt = buf.position()
        repeat(count) {
            if (buf.remaining() < 2) corrupt()
            val nameLen = buf.short.toInt() and 0xFFFF
            if (nameLen > MAX_NAME_BYTES || buf.remaining() < nameLen + 8) corrupt()
            buf.position(buf.position() + nameLen)
            val size = buf.long
            if (size < 0 || size > buf.remaining()) corrupt()
            buf.position(buf.position() + size.toInt())
        }
        if (buf.remaining() != 0) corrupt()
        // The actual envelope is the authority. A legacy one-member batch is still Free.
        if (count > 1 && !allowBatch) throw UnpackException(Failure.NeedsPro)
        buf.position(membersAt)
        val out = ArrayList<Pair<String, ByteArray>>(count)
        repeat(count) {
            val nameLen = buf.short.toInt() and 0xFFFF
            val name = ByteArray(nameLen).also { buf.get(it) }.toString(Charsets.UTF_8)
            val size = buf.long.toInt()
            out += name to ByteArray(size).also { buf.get(it) }
        }
        return out
    }

    fun isEnvelope(bytes: ByteArray): Boolean = hasMagic(bytes, MAGIC) || hasMagic(bytes, MAGIC_V1)

    private fun hasMagic(bytes: ByteArray, magic: ByteArray) =
        bytes.size >= magic.size && magic.indices.all { bytes[it] == magic[it] }

    /**
     * Decided per file, from a sample of the file itself. A quick compression of the first 256 KB
     * says whether the file is already compressed; if it is, the whole file is not touched, so a
     * photo costs a few milliseconds. If not, the whole file is compressed and kept only if it
     * really is smaller.
     */
    private fun compressIfWorthIt(data: ByteArray, mode: Mode, codec: Codec): ByteArray {
        if (mode == Mode.Off || data.size < MIN_COMPRESS_BYTES) return data
        val sample = if (data.size <= PROBE_BYTES) data else data.copyOf(PROBE_BYTES)
        val probe = codec.compress(sample, codec.probeLevel, longWindow = false) ?: return data
        if (probe.size > sample.size * PROBE_MAX_RATIO) return data
        val (level, longWindow) = when (mode) {
            // The strongest level's tables grow with the input: zstd 22 on a 64 MB file wants
            // several hundred MB, which a mid-range phone does not have to give. Past 16 MB, 19 --
            // within a few percent of 22, in a tenth of the memory.
            Mode.Maximum ->
                (if (data.size <= 16 * 1024 * 1024) codec.maxLevel else 19.coerceAtMost(codec.maxLevel)) to true
            // zstd 19 compresses a few MB/s on a phone; past 32 MB that starts to rival the time
            // it saves, so a little lighter. The long window only matters past its default size.
            else -> (if (data.size <= 32 * 1024 * 1024) 19 else 15).coerceAtMost(codec.maxLevel) to
                (data.size > 8 * 1024 * 1024)
        }
        val whole = codec.compress(data, level, longWindow) ?: return data
        return if (whole.size < data.size * KEEP_MAX_RATIO) whole else data
    }
}
