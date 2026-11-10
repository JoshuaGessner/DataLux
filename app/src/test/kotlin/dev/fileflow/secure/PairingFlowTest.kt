package dev.fileflow.secure

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Random

/**
 * Two phones on the pairing screen, played against each other: at each step one of them reads
 * whatever the other is showing -- or misses it -- in a random order, as two front cameras do.
 */
class PairingFlowTest {

    /** Runs reads until both are Done, or [steps] runs out. Returns the number of steps taken. */
    private fun run(a: PairingFlow, b: PairingFlow, seed: Long, miss: Double = 0.3, steps: Int = 400): Int {
        val r = Random(seed)
        repeat(steps) { i ->
            if (a.stage == PairingFlow.Stage.Done && b.stage == PairingFlow.Stage.Done) return i
            if (a.stage == PairingFlow.Stage.NeedsConfirmation) a.confirm()
            if (b.stage == PairingFlow.Stage.NeedsConfirmation) b.confirm()
            if (r.nextDouble() < miss) return@repeat
            if (r.nextBoolean()) a.read(b.qr) else b.read(a.qr)
        }
        return steps
    }

    @Test fun twoPhonesPairInAnyOrder() {
        repeat(200) { seed ->
            val a = PairingFlow(PairingFlow.Mode.Pair)
            val b = PairingFlow(PairingFlow.Mode.Pair)
            assertTrue("seed $seed", run(a, b, seed.toLong()) < 400)
            assertArrayEquals(a.key!!.secret.encoded, b.key!!.secret.encoded)
            assertEquals(a.key!!.code, b.key!!.code)
        }
    }

    @Test fun completedPeerKeepsItsAcknowledgementForASlowCamera() {
        val desktop = PairingFlow(PairingFlow.Mode.Pair)
        val phone = PairingFlow(PairingFlow.Mode.Pair)
        desktop.read(phone.qr)
        phone.read(desktop.qr)
        assertEquals(PairingFlow.Stage.Done, phone.stage)
        assertEquals(PairingFlow.Stage.GotKey, desktop.stage)
        val acknowledgement = phone.qr
        repeat(100) { phone.read(desktop.qr) }
        assertEquals(acknowledgement, phone.qr)
        desktop.read(phone.qr)
        assertEquals(PairingFlow.Stage.Done, desktop.stage)
        assertArrayEquals(phone.key!!.secret.encoded, desktop.key!!.secret.encoded)
    }

    @Test fun aTeamChangesHandsInAnyOrder() {
        repeat(200) { seed ->
            val team = Team.create("Test crew", myName = "Josh")
            val leader = PairingFlow(PairingFlow.Mode.Invite(team), "Josh")
            val member = PairingFlow(PairingFlow.Mode.Join, "Pixel")
            assertTrue("seed $seed: ${leader.stage} / ${member.stage}", run(leader, member, seed.toLong()) < 400)
            val joined = member.joined
            assertNotNull(joined)
            assertArrayEquals(team.secret.encoded, joined!!.secret.encoded)
            assertEquals(listOf("Josh", "Pixel"), joined.members.map { it.name })
            assertEquals("Pixel", leader.memberName)
            assertEquals(leader.key!!.code, member.key!!.code)
        }
    }

    @Test fun theJoinerShowsItsAcknowledgementUntilTheEnd() {
        val team = Team.create("Crew", myName = "A")
        val leader = PairingFlow(PairingFlow.Mode.Invite(team), "A")
        val member = PairingFlow(PairingFlow.Mode.Join, "B")
        run(leader, member, 1)
        assertTrue(member.qr.startsWith(TeamInvite.ACK_PREFIX))
        // Read again and again after the end: nothing changes, the code stays.
        repeat(5) { member.read(leader.qr) }
        assertTrue(member.qr.startsWith(TeamInvite.ACK_PREFIX))
    }

    @Test fun anotherPairingsTeamIsNotTaken() {
        // Another leader and member, done: that leader's screen shows its team, sealed to THEIR pairing.
        val otherLeader = PairingFlow(PairingFlow.Mode.Invite(Team.create("Other", myName = "C")), "C")
        run(otherLeader, PairingFlow(PairingFlow.Mode.Join, "D"), 2)
        assertTrue(otherLeader.qr.startsWith(TeamInvite.PREFIX))

        // A phone that has paired with nobody ignores it.
        val unpaired = PairingFlow(PairingFlow.Mode.Join, "E")
        unpaired.read(otherLeader.qr)
        assertEquals(PairingFlow.Stage.Scanning, unpaired.stage)

        // One paired with its own leader can't open it, and says so -- and still joins its own.
        val leader = PairingFlow(PairingFlow.Mode.Invite(Team.create("Crew", myName = "A")), "A")
        val member = PairingFlow(PairingFlow.Mode.Join, "B")
        member.read(leader.qr)
        leader.read(member.qr)
        member.confirm()
        leader.confirm()
        member.read(leader.qr)
        leader.read(member.qr)
        assertEquals(PairingFlow.Stage.HandingOver, leader.stage)
        assertTrue(member.read(otherLeader.qr))
        assertTrue(member.foreignInvite)
        assertEquals(null, member.joined)
        member.read(leader.qr)
        assertEquals("Crew", member.joined?.name)
    }

    @Test fun teamKeysRequireBothExplicitConfirmationsAndLegacyIsRejected() {
        val team = Team.create("Crew", myName = "A")
        val leader = PairingFlow(PairingFlow.Mode.Invite(team), "A")
        val member = PairingFlow(PairingFlow.Mode.Join, "B")
        val old = Pairing.start()
        assertTrue(leader.read(old.qrText()))
        assertTrue(member.read(old.qrText()))
        assertTrue(leader.legacyPeer)
        assertTrue(member.legacyPeer)
        assertEquals(null, leader.key)
        assertEquals(null, member.key)
        member.read(leader.qr)
        leader.read(member.qr)
        assertEquals(PairingFlow.Stage.NeedsConfirmation, leader.stage)
        assertEquals(PairingFlow.Stage.NeedsConfirmation, member.stage)
        assertTrue(!leader.qr.startsWith(TeamInvite.PREFIX))
        assertTrue(leader.confirm())
        member.read(leader.qr)
        assertTrue(!member.qr.startsWith(PairingFlow.CONFIRM_PREFIX))
        assertTrue(!leader.read(member.qr) || leader.stage != PairingFlow.Stage.HandingOver)
        assertEquals(null, member.joined)
        assertTrue(member.confirm())
        assertTrue(leader.read(member.qr))
        assertEquals(PairingFlow.Stage.HandingOver, leader.stage)
        assertTrue(member.read(leader.qr))
        assertArrayEquals(team.secret.encoded, member.joined!!.secret.encoded)
    }

    @Test fun unrelatedProofAndPrematureTeamDoNotUnlockHandover() {
        val leader = PairingFlow(PairingFlow.Mode.Invite(Team.create("Crew", myName = "A")), "A")
        val member = PairingFlow(PairingFlow.Mode.Join, "B")
        member.read(leader.qr)
        leader.read(member.qr)
        val fabricated = TeamInvite.encode(Team.create("Fake", myName = "X"), member.key!!)
        assertTrue(!member.read(fabricated))
        assertEquals(null, member.joined)
        assertTrue(leader.confirm())
        assertTrue(!leader.read("FFTCONF2:J:" + "00".repeat(32)))
        assertEquals(PairingFlow.Stage.WaitingForTeam, leader.stage)
        assertTrue(member.confirm())
        assertTrue(!member.read("FFTCONF2:L:" + "00".repeat(32)))
        assertTrue(!member.read(fabricated))
        assertEquals(null, member.joined)
    }

    @Test fun staleOrReplayedConfirmationCannotReleaseTeam() {
        var clock = 1_000L
        val team = Team.create("Crew", myName = "A")
        val leader = PairingFlow(PairingFlow.Mode.Invite(team), "A", now = { clock })
        val member = PairingFlow(PairingFlow.Mode.Join, "B", now = { clock })
        member.read(leader.qr)
        leader.read(member.qr)
        assertTrue(leader.confirm())
        assertTrue(member.confirm())
        assertTrue(member.read(leader.qr))
        val proof = member.qr
        val otherLeader = PairingFlow(PairingFlow.Mode.Invite(team), "A", now = { clock })
        val otherMember = PairingFlow(PairingFlow.Mode.Join, "C", now = { clock })
        otherMember.read(otherLeader.qr)
        otherLeader.read(otherMember.qr)
        assertTrue(otherLeader.confirm())
        assertTrue(!otherLeader.read(proof))
        clock += PairingStore.LIFETIME_MS
        assertTrue(!leader.read(proof))
        assertTrue(!leader.qr.startsWith(TeamInvite.PREFIX))
    }
}
