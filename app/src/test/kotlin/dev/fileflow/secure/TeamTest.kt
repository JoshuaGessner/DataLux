package dev.fileflow.secure

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class TeamTest {

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

    /** A member's copy of [t], as an invite would give it: equal, but not the same object. */
    private fun copyOf(t: Team) = Team(t.id.copyOf(), t.name, javax.crypto.spec.SecretKeySpec(t.secret.encoded, "AES"), t.createdMs)

    @Test fun everyMemberOpensWhatIsSentToTheTeam() {
        val team = Team.create("Field crew")
        val data = "grid reference 4471 2290".repeat(50).toByteArray()
        val p = Envelope.pack(data, "orders.txt", team)
        assertTrue(p.encrypted)
        repeat(3) {
            val member = copyOf(team)
            val u = Envelope.unpack(p.bytes, listOf(member), requireEncrypted = true)
            assertArrayEquals(data, u.data)
            assertEquals("orders.txt", u.name)
            assertSame(member, u.sealedTo)
        }
    }

    @Test fun theEnvelopesKeyIdPicksTheKeyAmongSeveral() {
        val (a, b) = paired()
        val crew = Team.create("Crew")
        val medics = Team.create("Medics")
        val keys = listOf(b, copyOf(crew), copyOf(medics))
        assertEquals("team Medics", Envelope.unpack(Envelope.pack("x".toByteArray(), "m", medics).bytes, keys, true).sealedTo?.label)
        assertEquals("paired phone", Envelope.unpack(Envelope.pack("x".toByteArray(), "p", a).bytes, keys, true).sealedTo?.label)
        val batch = Envelope.packBatch(listOf("1.txt" to "one".toByteArray(), "2.txt" to "two".toByteArray()), "2 files", crew)
        val u = Envelope.unpack(batch.bytes, keys, true)
        assertEquals(listOf("1.txt", "2.txt"), u.files!!.map { it.first })
        assertEquals("team Crew", u.sealedTo?.label)
    }

    @Test fun someoneOutsideTheTeamOpensNothing() {
        val team = Team.create("Crew")
        val p = Envelope.pack("secret".toByteArray(), "s", team)
        val (_, b) = paired()
        expectFailure(Envelope.Failure.WrongPairing) { Envelope.unpack(p.bytes, listOf(b, Team.create("Crew")), true) }
        expectFailure(Envelope.Failure.NeedsPairing) { Envelope.unpack(p.bytes, emptyList(), false) }
        // Same id, wrong key -- a forged team: the tag fails.
        val forged = Team(team.id, "Crew", Team.create("x").secret, 0)
        expectFailure(Envelope.Failure.WrongPairing) { Envelope.unpack(p.bytes, listOf(forged), true) }
    }

    @Test fun anInviteOpensOnlyOnThePhoneItWasPairedWith() {
        val (leader, member) = paired()
        val team = Team.create("Search & rescue")
        val invite = TeamInvite.encode(team, leader)
        val got = TeamInvite.decode(invite, member)
        assertNotNull(got)
        assertArrayEquals(team.id, got!!.id)
        assertArrayEquals(team.secret.encoded, got.secret.encoded)
        assertEquals("Search & rescue", got.name)
        assertEquals(team.createdMs, got.createdMs)

        // A camera across the room, paired with nobody -- or with someone else -- learns nothing.
        val (_, stranger) = paired()
        assertNull(TeamInvite.decode(invite, stranger))
        assertNull(TeamInvite.decode("FFPAIR1:abc", member))
        assertNull(TeamInvite.decode(TeamInvite.PREFIX + "!!!", member))
    }

    @Test fun anAlteredInviteIsRefused() {
        val (leader, member) = paired()
        val invite = TeamInvite.encode(Team.create("Crew"), leader)
        val body = java.util.Base64.getUrlDecoder().decode(invite.removePrefix(TeamInvite.PREFIX))
        for (i in body.indices step 7) {
            val bad = body.copyOf().also { it[i] = (it[i].toInt() xor 1).toByte() }
            val text = TeamInvite.PREFIX + java.util.Base64.getUrlEncoder().withoutPadding().encodeToString(bad)
            assertNull("byte $i", TeamInvite.decode(text, member))
        }
    }

    @Test fun onlyAPhoneHoldingTheTeamKeyCanAcknowledgeIt() {
        val (leader, member) = paired()
        val team = Team.create("Crew")
        val joined = TeamInvite.decode(TeamInvite.encode(team, leader), member)!!
        assertTrue(TeamInvite.isAck(TeamInvite.ack(joined, member), team, leader))
        assertFalse(TeamInvite.isAck(TeamInvite.ack(Team.create("Crew"), member), team, leader))
        val (_, other) = paired()
        assertFalse(TeamInvite.isAck(TeamInvite.ack(joined, other), team, leader))
        assertFalse(TeamInvite.isAck("FFTEAMOK:", team, leader))
    }

    private class MemoryStorage : TeamStore.Storage {
        var s: String? = null
        override fun load() = s
        override fun save(s: String) {
            this.s = s
        }
    }

    /** Stands in for the Keystore: reversible, and the stored bytes are not the key. */
    private object FlipWrap : TeamStore.KeyWrap {
        override fun wrap(key: ByteArray) = ByteArray(key.size) { (key[it].toInt() xor 0x5A).toByte() }
        override fun unwrap(wrapped: ByteArray) = wrap(wrapped)
    }

    @Test fun teamsAreKeptBetweenRunsWithTheirKeysWrapped() {
        val disk = MemoryStorage()
        val a = Team.create("Crew")
        val b = Team.create("Medics\tand\nnurses")
        TeamStore(disk, FlipWrap).apply { add(a); add(b); add(a) }

        val again = TeamStore(disk, FlipWrap).all()
        assertEquals(listOf(b.idHex, a.idHex), again.map { it.idHex })
        assertArrayEquals(a.secret.encoded, again[1].secret.encoded)
        assertEquals("Medicsandnurses", again[0].name)
        val keyB64 = java.util.Base64.getEncoder().encodeToString(a.secret.encoded)
        assertFalse("the key is never stored as it is", disk.s!!.contains(keyB64))

        TeamStore(disk, FlipWrap).remove(a.idHex)
        assertEquals(listOf(b.idHex), TeamStore(disk, FlipWrap).all().map { it.idHex })
    }

    @Test fun aTeamThatWillNotUnwrapIsDroppedNotFatal() {
        val disk = MemoryStorage()
        TeamStore(disk, FlipWrap).add(Team.create("Crew"))
        disk.s += "\nnot a team\n\t\t\t"
        val lost = object : TeamStore.KeyWrap {
            override fun wrap(key: ByteArray) = key
            override fun unwrap(wrapped: ByteArray): ByteArray = throw java.security.GeneralSecurityException("gone")
        }
        assertTrue(TeamStore(disk, lost).all().isEmpty())
        assertEquals(1, TeamStore(disk, FlipWrap).all().size)
    }

    @Test fun aTeamNeedsAName() {
        try {
            Team.create("  \n ")
            fail("expected a refusal")
        } catch (_: IllegalArgumentException) {
        }
        assertEquals(Team.MAX_NAME_CHARS, Team.create("x".repeat(100)).name.length)
    }
}
