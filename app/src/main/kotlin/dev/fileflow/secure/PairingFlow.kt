package dev.fileflow.secure

/**
 * What one phone does on the pairing screen, as codes are read: which code it shows, and how far
 * it has got. Kept apart from the screen so that two of them can be played against each other in a
 * test -- reads in any order, any missed -- which is how the team hand-over is checked.
 *
 * [Mode.Pair] is plain pairing ([Pairing]). [Mode.Invite] pairs, then shows the team's key sealed
 * to that pairing ([TeamInvite]) until the new member's acknowledgement comes back. [Mode.Join]
 * pairs, then waits for that sealed key and answers with its own acknowledgement.
 */
class PairingFlow(
    private val mode: Mode,
    /** This phone's person, as their name goes to a team's leader on joining. */
    private val myName: String = "",
    private val pairing: Pairing = Pairing.start(),
    private val now: () -> Long = System::currentTimeMillis,
) {
    sealed interface Mode {
        data object Pair : Mode
        class Invite(val team: Team) : Mode
        data object Join : Mode
    }

    enum class Stage {
        /** Showing this phone's key, waiting to read the other's. */
        Scanning,
        /** Has the other phone's key -- the six-digit code can be shown -- not yet confirmed both ways. */
        GotKey,
        /** Invite: both phones hold the pairing key, and the team key is on screen for the member. */
        HandingOver,
        /** Join: paired, and waiting for the leader's screen to show the team. */
        WaitingForTeam,
        /** Pair: both ways confirmed. Invite: the member has joined. Join: this phone has joined. */
        Done,
    }

    var stage = Stage.Scanning
        private set

    /** The pairing key, once the other phone's key has been read. */
    var key: PairingKey? = null
        private set

    /** What this phone's screen shows now. */
    var qr: String = pairing.qrText()
        private set

    /** Join: the team now on this phone. */
    var joined: Team? = null
        private set

    /** Invite: the name the new member gave, once they have joined. */
    var memberName: String? = null
        private set

    /** Set when a team code was read that this pairing cannot open: someone else's, mid-swap. */
    var foreignInvite = false
        private set

    /** Takes one decoded code. True when anything changed -- the screen should be redrawn. */
    fun read(text: String): Boolean {
        if (stage == Stage.Done) return false
        if (TeamInvite.isTeamCode(text)) return readTeamCode(text)
        return when (pairing.read(text)) {
            Pairing.Read.NotOurs, Pairing.Read.Own -> false
            Pairing.Read.PeerKey -> gotKey(confirmed = false)
            Pairing.Read.PeerComplete -> gotKey(confirmed = true)
        }
    }

    private fun gotKey(confirmed: Boolean): Boolean {
        val firstKey = key == null
        if (firstKey) key = pairing.key(now())
        val k = key ?: return false
        val before = stage
        // Our code now acknowledges theirs -- which is what completes THEIR side -- unless it has
        // moved on to the team key, which must stay up until the member reads it.
        if (stage != Stage.HandingOver) qr = pairing.qrText()
        stage = when {
            !confirmed -> if (stage == Stage.Scanning) Stage.GotKey else stage
            mode is Mode.Pair -> Stage.Done
            mode is Mode.Invite -> {
                if (stage != Stage.HandingOver) qr = TeamInvite.encode(mode.team, k)
                Stage.HandingOver
            }
            else -> if (stage == Stage.Scanning || stage == Stage.GotKey) Stage.WaitingForTeam else stage
        }
        return firstKey || stage != before
    }

    private fun readTeamCode(text: String): Boolean {
        val k = key ?: return false
        when (mode) {
            is Mode.Invite -> {
                val name = TeamInvite.readAck(text, mode.team, k) ?: return false
                memberName = name
                stage = Stage.Done
                return true
            }
            Mode.Join -> {
                if (!text.startsWith(TeamInvite.PREFIX)) return false
                // The leader shows the team only once it has OUR acknowledgement of ITS key, so a
                // joiner can see the team before it has seen the leader's acknowledgement of ours.
                // Opening the team proves the leader holds the same pairing key; nothing else is needed.
                val team = TeamInvite.decode(text, k, myName, now())
                if (team == null) {
                    if (!foreignInvite) {
                        foreignInvite = true
                        return true
                    }
                    return false
                }
                joined = team
                qr = TeamInvite.ack(team, k, myName)
                stage = Stage.Done
                return true
            }
            Mode.Pair -> return false
        }
    }
}
