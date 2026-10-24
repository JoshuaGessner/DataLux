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
}

class PairingTest {

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
