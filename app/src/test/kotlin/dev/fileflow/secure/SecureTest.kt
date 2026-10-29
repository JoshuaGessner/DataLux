package dev.fileflow.secure

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.util.Random

class EnvelopeTest {

    private fun text(n: Int): ByteArray {
        val words = listOf("light ", "camera ", "screen ", "fountain ", "symbol ", "frame ")
        val r = Random(7)
        return buildString { while (length < n) append(words[r.nextInt(words.size)]) }
            .take(n).toByteArray()
    }

    private fun noise(n: Int) = ByteArray(n).also { Random(9).nextBytes(it) }

    private fun paired(): Pair<PairingKey, PairingKey> {
        val a = Pairing.start()
        val b = Pairing.start()
        b.read(a.qrText())
        a.read(b.qrText())
        return a.key()!! to b.key()!!
    }

    private fun expectFailure(f: Envelope.Failure, block: () -> Unit) {
        try {
            block()
            fail("expected $f")
        } catch (e: Envelope.UnpackException) {
            assertEquals(f, e.failure)
        }
    }

    @Test fun textIsCompressedAndComesBackExactly() {
        val data = text(200_000)
        val p = Envelope.pack(data, "notes.txt", null)
        assertTrue(p.compressed)
        assertTrue("text should shrink several times, got ${p.bytes.size}", p.bytes.size < data.size / 3)
        val u = Envelope.unpack(p.bytes, null, requireEncrypted = false)
        assertEquals("notes.txt", u.name)
        assertArrayEquals(data, u.data)
    }

    @Test fun alreadyCompressedDataIsSentAsIs() {
        val data = noise(300_000)  // stands in for a JPEG or an MP4
        val p = Envelope.pack(data, "photo.jpg", null)
        assertFalse(p.compressed)
        assertTrue(p.bytes.size - data.size < 64)
        assertArrayEquals(data, Envelope.unpack(p.bytes, null, false).data)
    }

    @Test fun encryptedRoundTripHidesTheNameAndTheContent() {
        val (a, b) = paired()
        val data = text(50_000)
        val p = Envelope.pack(data, "secret plans.txt", a)
        assertTrue(p.encrypted)
        assertFalse(String(p.bytes, Charsets.ISO_8859_1).contains("secret plans"))
        assertFalse(String(p.bytes, Charsets.ISO_8859_1).contains("fountain"))
        val u = Envelope.unpack(p.bytes, b, requireEncrypted = true)
        assertEquals("secret plans.txt", u.name)
        assertArrayEquals(data, u.data)
        assertTrue(u.encrypted)
    }

    @Test fun anotherPairingCannotOpenIt() {
        val (a, _) = paired()
        val (_, other) = paired()
        val p = Envelope.pack(text(1000), "a.txt", a)
        expectFailure(Envelope.Failure.WrongPairing) { Envelope.unpack(p.bytes, other, true) }
        expectFailure(Envelope.Failure.NeedsPairing) { Envelope.unpack(p.bytes, null, false) }
    }

    @Test fun anyAlteredByteIsCaught() {
        val (a, b) = paired()
        val p = Envelope.pack(text(5000), "a.txt", a)
        for (i in listOf(5, 20, 40, p.bytes.size / 2, p.bytes.size - 1)) {
            val bad = p.bytes.copyOf().also { it[i] = (it[i].toInt() xor 1).toByte() }
            try {
                Envelope.unpack(bad, b, true)
                fail("byte $i altered and still accepted")
            } catch (e: Envelope.UnpackException) {
                // WrongPairing or Corrupt: either way, nothing is delivered.
            }
        }
    }

    @Test fun encryptedModeRefusesAPlainFile() {
        val p = Envelope.pack(text(1000), "a.txt", null)
        expectFailure(Envelope.Failure.NotEncrypted) { Envelope.unpack(p.bytes, null, requireEncrypted = true) }
    }

    @Test fun aSizeLieCannotInflateIntoABomb() {
        val p = Envelope.pack(ByteArray(1_000_000), "zeros.bin", null)
        assertTrue(p.compressed)
        // Claim fewer bytes than the stream really holds: refused, not truncated or overrun.
        val bad = p.bytes.copyOf()
        val sizeAt = 4 + 1 + 2 + "zeros.bin".length
        bad[sizeAt + 5] = 0  // 1,000,000 -> a smaller number
        expectFailure(Envelope.Failure.Corrupt) { Envelope.unpack(bad, null, false) }
    }

    @Test fun emptyAndTinyFilesSurvive() {
        for (n in listOf(0, 1, 511, 512, 513)) {
            val data = text(n)
            assertArrayEquals(data, Envelope.unpack(Envelope.pack(data, "t", null).bytes, null, false).data)
        }
    }

    @Test fun notAnEnvelopeIsSaidSo() {
        expectFailure(Envelope.Failure.NotAnEnvelope) { Envelope.unpack("hello".toByteArray(), null, false) }
    }

    @Test fun offSendsTheFileAsItIs() {
        val data = text(100_000)
        val p = Envelope.pack(data, "notes.txt", null, Envelope.Mode.Off)
        assertFalse(p.compressed)
        assertEquals(0L, p.compressMs)
        assertTrue(p.bytes.size - data.size < 64)
        val u = Envelope.unpack(p.bytes, null, false)
        assertEquals(Envelope.CODEC_NONE, u.codec)
        assertArrayEquals(data, u.data)
    }

    @Test fun theCodecAndItsCostTravelWithTheFile() {
        val data = text(200_000)
        for (mode in listOf(Envelope.Mode.Auto, Envelope.Mode.Maximum)) {
            val p = Envelope.pack(data, "notes.txt", null, mode, DeflateCodec)
            assertEquals(DeflateCodec.id, p.codec)
            val u = Envelope.unpack(p.bytes, null, false)
            assertEquals(DeflateCodec.id, u.codec)
            assertEquals(p.compressMs, u.compressMs)
            assertArrayEquals(data, u.data)
        }
    }

    @Test fun maximumIsNoBiggerThanAuto() {
        val data = text(300_000)
        val auto = Envelope.pack(data, "a", null, Envelope.Mode.Auto, DeflateCodec)
        val max = Envelope.pack(data, "a", null, Envelope.Mode.Maximum, DeflateCodec)
        assertTrue(max.bytes.size <= auto.bytes.size)
    }

    @Test fun aVersionOneEnvelopeStillOpens() {
        // "FFE1" | flags (1 = compressed) | name length | name | original size | raw Deflate
        val data = text(20_000)
        val body = DeflateCodec.compress(data, 9, false)!!
        val name = "old.txt".toByteArray()
        val v1 = java.nio.ByteBuffer.allocate(4 + 1 + 2 + name.size + 8 + body.size)
            .put("FFE1".toByteArray()).put(1).putShort(name.size.toShort()).put(name)
            .putLong(data.size.toLong()).put(body).array()
        val u = Envelope.unpack(v1, null, false)
        assertEquals("old.txt", u.name)
        assertEquals(DeflateCodec.id, u.codec)
        assertArrayEquals(data, u.data)
    }

    @Test fun aBatchComesBackAsItsFilesInOrder() {
        val files = listOf("a.txt" to text(40_000), "empty.bin" to ByteArray(0), "photo.jpg" to noise(30_000))
        for (key in listOf<PairingKey?>(null, paired().first)) {
            val p = Envelope.packBatch(files, "3 files", key, Envelope.Mode.Auto, DeflateCodec)
            val reader = if (key == null) null else key
            val u = Envelope.unpack(p.bytes, reader, requireEncrypted = key != null)
            val got = u.files
            assertNotNull(got)
            assertEquals(files.map { it.first }, got!!.map { it.first })
            files.zip(got).forEach { (want, have) -> assertArrayEquals(want.second, have.second) }
            assertEquals("3 files", u.name)
        }
    }

    @Test fun aSingleFileIsNotABatch() {
        val u = Envelope.unpack(Envelope.pack(text(1000), "a.txt", null).bytes, null, false)
        assertNull(u.files)
    }

    @Test fun freeReceiverAcceptsSingleFilesWithAndWithoutEncryption() {
        for (key in listOf<PairingKey?>(null, paired().first)) {
            for (mode in Envelope.Mode.entries) {
                val data = text(20_000)
                val packed = Envelope.pack(data, "single.txt", key, mode, DeflateCodec)
                val opened = Envelope.unpack(packed.bytes, listOfNotNull(key), key != null, allowBatch = false)
                assertNull(opened.files)
                assertArrayEquals(data, opened.data)
            }
        }
    }

    @Test fun batchRequiresReceivingEntitlementRegardlessOfCompressionOrPairing() {
        val files = listOf("a.txt" to text(20_000), "b.txt" to text(10_000))
        for (key in listOf<PairingKey?>(null, paired().first)) {
            for (mode in Envelope.Mode.entries) {
                val packed = Envelope.packBatch(files, "2 files", key, mode, DeflateCodec)
                expectFailure(Envelope.Failure.NeedsPro) {
                    Envelope.unpack(packed.bytes, listOfNotNull(key), key != null, allowBatch = false)
                }
                val restored = Envelope.unpack(packed.bytes, listOfNotNull(key), key != null, allowBatch = true)
                val openedFiles = restored.files!!
                assertEquals(files.map { it.first }, openedFiles.map { it.first })
                files.zip(openedFiles).forEach { (want, got) -> assertArrayEquals(want.second, got.second) }
            }
        }
    }

    @Test fun anUnauthenticatedEncryptedBatchDoesNotBecomeAnUpgradePrompt() {
        val (sender, _) = paired()
        val (foreign, _) = paired()
        val packed = Envelope.packBatch(listOf("a" to text(100), "b" to text(100)), "2 files", sender)
        expectFailure(Envelope.Failure.WrongPairing) {
            Envelope.unpack(packed.bytes, listOf(foreign), true, allowBatch = false)
        }
        val altered = packed.bytes.copyOf().also { it[it.lastIndex] = (it.last().toInt() xor 1).toByte() }
        expectFailure(Envelope.Failure.WrongPairing) {
            Envelope.unpack(altered, listOf(sender), true, allowBatch = false)
        }
    }

    @Test fun aLegacySingleFileBatchContainerRemainsFree() {
        val bytes = text(1000)
        val packed = Envelope.packBatch(listOf("one.txt" to bytes), "1 file", null)
        val opened = Envelope.unpack(packed.bytes, emptyList(), false, allowBatch = false)
        assertEquals(1, opened.files!!.size)
        assertArrayEquals(bytes, opened.files.single().second)
    }

    @Test fun aMalformedBatchDoesNotSellAnUpgradeBeforeReportingCorruption() {
        val truncated = "FFB1".toByteArray() + byteArrayOf(0, 2)
        val packed = Envelope.pack(truncated, "2 files", null, Envelope.Mode.Off).bytes
        packed[4] = 4 // A valid envelope claiming a batch, but no member records exist.
        expectFailure(Envelope.Failure.Corrupt) {
            Envelope.unpack(packed, emptyList(), false, allowBatch = false)
        }
    }

    @Test fun aBatchThatLiesAboutItsSizesIsRefused() {
        val p = Envelope.packBatch(listOf("a" to text(100), "b" to text(100)), "2 files", null, Envelope.Mode.Off)
        // Plain, uncompressed: the batch starts right after the inner header. Claim the first file
        // is far longer than the batch -- the bound, not the claim, must win.
        val bad = p.bytes.copyOf()
        val batchAt = 4 + 1 + 2 + "2 files".length + 8 + 1 + 4
        val firstSizeAt = batchAt + 4 + 2 + 2 + 1
        bad[firstSizeAt + 4] = 0x7F
        expectFailure(Envelope.Failure.Corrupt) { Envelope.unpack(bad, null, false) }
    }

    @Test fun anUnknownCodecIsSaidSo() {
        val p = Envelope.pack(text(10_000), "a.txt", null, Envelope.Mode.Auto, DeflateCodec)
        val bad = p.bytes.copyOf()
        bad[4 + 1 + 2 + "a.txt".length + 8] = 9  // the codec byte
        expectFailure(Envelope.Failure.UnknownCodec) { Envelope.unpack(bad, null, false) }
    }

    // What the desktop sender's core/src/envelope.cpp makes, from core/tests/test_envelope.cpp: the
    // two implementations are held to one shared vector.
    @Test fun desktopEnvelopesOpen() {
        fun bytes(hex: String) = ByteArray(hex.length / 2) { hex.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
        val one = Envelope.unpack(bytes("4646453200" + "0005" + "782e747874" + "0000000000000005" + "00" +
            "00000000" + "68656c6c6f"), null, false)
        assertEquals("x.txt", one.name)
        assertEquals("hello", one.data.toString(Charsets.UTF_8))
        val batch = Envelope.unpack(bytes("4646453204" + "0007" + "322066696c6573" + "0000000000000023" + "00" +
            "00000000" + "46464231" + "0002" + "0005" + "612e747874" + "0000000000000002" + "6869" +
            "0001" + "62" + "0000000000000001" + "21"), null, false)
        assertEquals("2 files", batch.name)
        assertEquals(listOf("a.txt" to "hi", "b" to "!"), batch.files!!.map { (n, b) -> n to b.toString(Charsets.UTF_8) })
    }
}

class PairingTest {

    @Test fun expiredPairingsCannotBeRevivedByAClockChange() {
        val a = Pairing.start()
        a.read(Pairing.start().qrText())
        val created = 10_000L
        val key = a.key(created)!!
        try {
            PairingStore.set(key)
            assertEquals(key, PairingStore.current(created + PairingStore.LIFETIME_MS - 1))
            assertNull(PairingStore.current(created + PairingStore.LIFETIME_MS))
            assertNull(PairingStore.current(created))
            PairingStore.set(key)
            assertNull(PairingStore.current(created - 1))
            assertNull(PairingStore.current(created))
        } finally {
            PairingStore.forget()
        }
    }

    @Test fun bothPhonesDeriveTheSameKeyAndCode() {
        val a = Pairing.start()
        val b = Pairing.start()
        assertEquals(Pairing.Read.PeerKey, b.read(a.qrText()))
        // b's code now acknowledges a's key, so a learns the exchange is complete on both sides.
        assertEquals(Pairing.Read.PeerComplete, a.read(b.qrText()))
        assertEquals(Pairing.Read.PeerComplete, b.read(a.qrText()))
        val ka = a.key()!!
        val kb = b.key()!!
        assertArrayEquals(ka.secret.encoded, kb.secret.encoded)
        assertArrayEquals(ka.id, kb.id)
        assertEquals(ka.code, kb.code)
        assertTrue(ka.code.matches(Regex("\\d{3} \\d{3}")))
    }

    @Test fun ownCodeAndStrangersAreIgnored() {
        val a = Pairing.start()
        assertEquals(Pairing.Read.Own, a.read(a.qrText()))
        assertEquals(Pairing.Read.NotOurs, a.read("https://example.com"))
        assertEquals(Pairing.Read.NotOurs, a.read(Pairing.PREFIX + "not-base64!"))
        assertNull(a.key())
    }

    @Test fun aPointOffTheCurveIsRefused() {
        val a = Pairing.start()
        val bogus = ByteArray(65).also { it[0] = 4; it[64] = 1 }
        val text = Pairing.PREFIX + java.util.Base64.getUrlEncoder().withoutPadding().encodeToString(bogus)
        assertEquals(Pairing.Read.NotOurs, a.read(text))
    }

    @Test fun aThirdPhoneCannotJoinAPairingInProgress() {
        val a = Pairing.start()
        val b = Pairing.start()
        val c = Pairing.start()
        a.read(b.qrText())
        assertEquals(Pairing.Read.NotOurs, a.read(c.qrText()))
        val ab = a.key()
        b.read(a.qrText())
        assertNotNull(ab)
        assertArrayEquals(ab!!.secret.encoded, b.key()!!.secret.encoded)
    }

    @Test fun differentPairingsGiveDifferentKeys() {
        val a1 = Pairing.start(); val b1 = Pairing.start()
        val a2 = Pairing.start(); val b2 = Pairing.start()
        b1.read(a1.qrText()); a1.read(b1.qrText())
        b2.read(a2.qrText()); a2.read(b2.qrText())
        assertFalse(a1.key()!!.secret.encoded.contentEquals(a2.key()!!.secret.encoded))
    }
}
