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
        assertEquals(PairingFlow.Stage.HandingOver, leader.stage)
        assertTrue(member.read(otherLeader.qr))
        assertTrue(member.foreignInvite)
        assertEquals(null, member.joined)
        member.read(leader.qr)
        assertEquals("Crew", member.joined?.name)
    }
}
