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
        val team = Team.create("Search & rescue", myName = "Alex", nowMs = 1000)
            .withMember(Member("Sam", 2000))
        val invite = TeamInvite.encode(team, leader)
        val got = TeamInvite.decode(invite, member, "Jo", nowMs = 3000)
        assertNotNull(got)
        assertArrayEquals(team.id, got!!.id)
        assertArrayEquals(team.secret.encoded, got.secret.encoded)
        assertEquals("Search & rescue", got.name)
        assertEquals(team.createdMs, got.createdMs)
        // The member's view: led by Alex, Sam already in, and this phone itself.
        assertFalse(got.isLeader)
        assertEquals(listOf("Alex", "Sam", "Jo"), got.members.map { it.name })
        assertEquals(listOf(true, false, false), got.members.map { it.leader })
        assertEquals(listOf(false, false, true), got.members.map { it.you })

        // A camera across the room, paired with nobody -- or with someone else -- learns nothing.
        val (_, stranger) = paired()
        assertNull(TeamInvite.decode(invite, stranger, "Eve"))
        assertNull(TeamInvite.decode("FFPAIR1:abc", member, "Jo"))
        assertNull(TeamInvite.decode(TeamInvite.PREFIX + "!!!", member, "Jo"))
    }

    @Test fun anInviteListsOnlyWhoHoldsTheCurrentKey() {
        val (leader, member) = paired()
        var team = Team.create("Crew", myName = "Alex")
        repeat(20) { team = team.withMember(Member("m$it", it.toLong())) }
        val got = TeamInvite.decode(TeamInvite.encode(team, leader), member, "Jo")!!
        // The newest MAX_LISTED, so the code stays small enough to read.
        assertEquals(listOf("Alex") + (8 until 20).map { "m$it" } + "Jo", got.members.map { it.name })
        val rotated = team.rotated()
        val after = TeamInvite.decode(TeamInvite.encode(rotated, leader), member, "Jo")!!
        assertEquals(listOf("Alex", "Jo"), after.members.map { it.name })
    }

    @Test fun anAlteredInviteIsRefused() {
        val (leader, member) = paired()
        val invite = TeamInvite.encode(Team.create("Crew"), leader)
        val body = java.util.Base64.getUrlDecoder().decode(invite.removePrefix(TeamInvite.PREFIX))
        for (i in body.indices step 7) {
            val bad = body.copyOf().also { it[i] = (it[i].toInt() xor 1).toByte() }
            val text = TeamInvite.PREFIX + java.util.Base64.getUrlEncoder().withoutPadding().encodeToString(bad)
            assertNull("byte $i", TeamInvite.decode(text, member, "Jo"))
        }
    }

    @Test fun onlyAPhoneHoldingTheTeamKeyCanAcknowledgeIt() {
        val (leader, member) = paired()
        val team = Team.create("Crew", myName = "Alex")
        val joined = TeamInvite.decode(TeamInvite.encode(team, leader), member, "Jo")!!
        assertEquals("Jo", TeamInvite.readAck(TeamInvite.ack(joined, member, "Jo"), team, leader))
        assertNull(TeamInvite.readAck(TeamInvite.ack(Team.create("Crew"), member, "Jo"), team, leader))
        val (_, other) = paired()
        assertNull(TeamInvite.readAck(TeamInvite.ack(joined, other, "Jo"), team, leader))
        assertNull(TeamInvite.readAck(TeamInvite.ACK_PREFIX, team, leader))
        assertNull(TeamInvite.readAck(TeamInvite.encode(team, leader), team, leader))
    }

    @Test fun removingSomeoneChangesTheKeyAndAsksForEveryoneElseAgain() {
        val team = Team.create("Crew", myName = "Alex").withMember(Member("Sam", 1)).withMember(Member("Jo", 2))
        val sam = team.members.first { it.name == "Sam" }
        val after = team.without(sam).rotated()
        assertFalse(after.id.contentEquals(team.id))
        assertFalse(after.secret.encoded.contentEquals(team.secret.encoded))
        assertEquals(listOf("Alex", "Jo"), after.members.map { it.name })
        assertEquals(listOf(false, true), after.members.map { it.needsNewKey })
        // Jo joining again takes the place of the waiting entry.
        val back = after.withMember(Member("Jo", 3))
        assertEquals(listOf("Alex", "Jo"), back.members.map { it.name })
        assertFalse(back.members.last().needsNewKey)
        // Files sealed to the old key do not open with the new one.
        val old = Envelope.pack("before".toByteArray(), "a", team)
        expectFailure(Envelope.Failure.WrongPairing) { Envelope.unpack(old.bytes, listOf<SealKey>(after), true) }
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

    @Test fun membersAndRoleAreKept() {
        val disk = MemoryStorage()
        val led = Team.create("Crew", myName = "Alex", nowMs = 5).withMember(Member("Sam", 6))
        val (leader, member) = paired()
        val joined = TeamInvite.decode(TeamInvite.encode(led, leader), member, "Jo", nowMs = 7)!!
        TeamStore(disk, FlipWrap).apply { add(led); add(joined.rotated()) }
        val (a, b) = TeamStore(disk, FlipWrap).all()
        assertTrue(a.isLeader)
        assertEquals(listOf("Alex", "Sam"), a.members.map { it.name })
        assertEquals(listOf(true, false), a.members.map { it.you })
        assertFalse(b.isLeader)
        assertEquals(listOf("Alex", "Sam", "Jo"), b.members.map { it.name })
        assertEquals(listOf(false, true, false), b.members.map { it.needsNewKey })
        assertEquals(listOf(5L, 6L, 7L), b.members.map { it.joinedMs })
    }

    @Test fun aTeamSavedBeforeMembersExistedStillOpens() {
        val disk = MemoryStorage()
        val t = Team.create("Test crew")
        val e = java.util.Base64.getEncoder()
        // The line the first release of teams wrote: four fields.
        disk.s = listOf(e.encodeToString(t.id), e.encodeToString("Test crew".toByteArray()),
                        e.encodeToString(FlipWrap.wrap(t.secret.encoded)), "1792891803122").joinToString("\t")
        val got = TeamStore(disk, FlipWrap).all().single()
        assertEquals("Test crew", got.name)
        assertTrue(got.isLeader)
        assertTrue(got.members.isEmpty())
        assertArrayEquals(t.secret.encoded, got.secret.encoded)
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
