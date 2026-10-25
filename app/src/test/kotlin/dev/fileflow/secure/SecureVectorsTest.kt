package dev.fileflow.secure

import org.junit.Assert.assertEquals
import org.junit.Test
import java.security.SecureRandom
import javax.crypto.spec.SecretKeySpec

/**
 * Fixed vectors for the secure formats -- pairing keys, sealed envelopes, team invites and their
 * acknowledgements -- that the desktop app's C++ implementation (desktop/tests/test_secure.cpp) is
 * held to as well: the same inputs must give the same bytes on both, or a phone and a computer could
 * not open each other's files.
 */
class SecureVectorsTest {

    /** "Random" bytes that are not: seed, seed + 1, ... -- so a nonce is fixed and known. */
    private class Fixed(private val seed: Int) : SecureRandom() {
        override fun nextBytes(bytes: ByteArray) {
            for (i in bytes.indices) bytes[i] = (seed + i).toByte()
        }
    }

    private fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }
    private fun bytes(n: Int, f: (Int) -> Int) = ByteArray(n) { f(it).toByte() }

    private val shared = bytes(32) { it + 1 }
    private val mine = bytes(65) { if (it == 0) 4 else it }
    private val peer = bytes(65) { if (it == 0) 4 else 65 - it }

    @Test fun pairingKeyDerivation() {
        val k = Pairing.derive(shared, mine, peer, 0)
        // Either side's view gives the same key.
        val other = Pairing.derive(shared, peer, mine, 0)
        assertEquals(hex(k.secret.encoded), hex(other.secret.encoded))
        assertEquals(VECTOR_KEY, hex(k.secret.encoded))
        assertEquals(VECTOR_ID, hex(k.id))
        assertEquals(VECTOR_CODE, k.code)
    }

    @Test fun sealedEnvelope() {
        val key = PairingKey(SecretKeySpec(bytes(32) { it * 7 + 3 }, "AES"), bytes(8) { 0xA0 + it }, "", 0)
        val p = Envelope.pack("hello, team".toByteArray(), "note.txt", key, Envelope.Mode.Off, DeflateCodec, Fixed(0x10))
        assertEquals(VECTOR_ENVELOPE, hex(p.bytes))
        assertEquals("hello, team", Envelope.unpack(p.bytes, key, true).data.toString(Charsets.UTF_8))
    }

    @Test fun teamInviteAndAck() {
        val pairing = Pairing.derive(shared, mine, peer, 0)
        val team = Team(bytes(8) { 0xB0 + it }, "Crew", SecretKeySpec(bytes(32) { 0xC0 + it }, "AES"), 1_700_000_000_000,
            members = listOf(Member("Alex", 1_700_000_000_000, leader = true, you = true), Member("Sam", 1_700_000_001_000)))
        val invite = TeamInvite.encode(team, pairing, Fixed(0x20))
        assertEquals(VECTOR_INVITE, invite)
        val joined = TeamInvite.decode(invite, pairing, "Jo", 5)!!
        assertEquals(VECTOR_ACK, TeamInvite.ack(joined, pairing, "Jo", Fixed(0x30)))
    }

    companion object {
        const val VECTOR_KEY = "9e644ddfbe4679dc930bf73ea9c90216935b54ab9360d48b4908b27ba5a6fc24"
        const val VECTOR_ID = "99fd1bb27debc69f"
        const val VECTOR_CODE = "187 732"
        const val VECTOR_ENVELOPE = "4646453202a0a1a2a3a4a5a6a7101112131415161718191a1bbe8cbdbf0eed6b97abc62b491373cb4310e9870d0138476365aae56d4879c0de083f18a7b3e262f5702eaa9f8653f713c199"
        const val VECTOR_INVITE = "FFTEAM2:ICEiIyQlJicoKSorQAkX_H8Suopkh3IaA9P-N0FJQIZy12MdrnAONeqx5dCSL7Eji_e5FYJZfPl3kT7Zv-5RHmT6Al9NFvm7r_9hnlTHcXa0-l-05HMrXTPAp2GbIving3GjS2w"
        const val VECTOR_ACK = "FFTEAMOK2:MDEyMzQ1Njc4OTo7SpcVy78k5blkYd2pCs0A1FG2Cit8acjFlTy3"
    }
}
