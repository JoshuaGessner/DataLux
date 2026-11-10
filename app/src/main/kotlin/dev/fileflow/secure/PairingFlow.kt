package dev.fileflow.secure

import java.security.MessageDigest
import javax.crypto.Mac

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
        /** The key exchange is ready for the person to compare and confirm the code. */
        NeedsConfirmation,
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
    var qr: String = pairingQr()
        private set

    private var peerComplete = false
    private var localConfirmed = false
    private var leaderConfirmed = false

    private fun keyIsFresh(k: PairingKey): Boolean {
        val at = now()
        return at >= k.createdMs && at - k.createdMs < PairingStore.LIFETIME_MS
    }

    private fun pairingQr(): String = if (mode is Mode.Pair) pairing.qrText()
        else TEAM_PAIR_PREFIX + pairing.qrText().removePrefix(Pairing.PREFIX)

    internal fun proof(role: String, k: PairingKey): String {
        val keyId = k.id.joinToString("") { "%02x".format(it.toInt() and 0xff) }
        val mac = Mac.getInstance("HmacSHA256").apply { init(k.secret) }
        val bytes = mac.doFinal("DataLux team confirm v2:$role:$keyId".toByteArray(Charsets.UTF_8))
        return CONFIRM_PREFIX + role + ":" + bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }

    /** Called only after the user compared both visible codes and explicitly agreed. */
    fun confirm(): Boolean {
        val k = key ?: return false
        if (!keyIsFresh(k)) return false
        if (mode is Mode.Pair || localConfirmed || (mode is Mode.Invite && !peerComplete)) return false
        localConfirmed = true
        qr = when (mode) {
            is Mode.Invite -> proof("L", k)
            Mode.Join -> if (leaderConfirmed) proof("J", k) else pairingQr()
            else -> return false
        }
        stage = Stage.WaitingForTeam
        return true
    }

    /** Join: the team now on this phone. */
    var joined: Team? = null
        private set

    /** Invite: the name the new member gave, once they have joined. */
    var memberName: String? = null
        private set

    /** Set when a team code was read that this pairing cannot open: someone else's, mid-swap. */
    var foreignInvite = false
        private set

    var legacyPeer = false
        private set

    /** Takes one decoded code. True when anything changed -- the screen should be redrawn. */
    fun read(text: String): Boolean {
        if (stage == Stage.Done) return false
        if (mode !is Mode.Pair && key?.let { !keyIsFresh(it) } == true) return false
        if (mode !is Mode.Pair && text.startsWith(Pairing.PREFIX)) {
            if (legacyPeer) return false
            legacyPeer = true
            return true
        }
        if (mode !is Mode.Pair && text.startsWith(CONFIRM_PREFIX)) return readConfirmation(text)
        if (TeamInvite.isTeamCode(text)) return readTeamCode(text)
        val pairText = if (mode is Mode.Pair) text else {
            if (!text.startsWith(TEAM_PAIR_PREFIX)) return false
            Pairing.PREFIX + text.removePrefix(TEAM_PAIR_PREFIX)
        }
        return when (pairing.read(pairText)) {
            Pairing.Read.NotOurs, Pairing.Read.Own -> false
            Pairing.Read.PeerKey -> gotKey(confirmed = false)
            Pairing.Read.PeerComplete -> gotKey(confirmed = true)
        }
    }

    private fun gotKey(confirmed: Boolean): Boolean {
        val firstKey = key == null
        if (firstKey) key = pairing.key(now())
        if (key == null) return false
        val before = stage
        if (mode !is Mode.Pair) {
            peerComplete = peerComplete || confirmed
            if (!localConfirmed) {
                qr = pairingQr()
                stage = if (mode is Mode.Join || peerComplete) Stage.NeedsConfirmation else Stage.GotKey
            } else if (mode is Mode.Join && !leaderConfirmed) {
                qr = pairingQr()
            }
            return firstKey || stage != before
        }
        qr = pairing.qrText()
        stage = if (confirmed) Stage.Done else if (stage == Stage.Scanning) Stage.GotKey else stage
        return firstKey || stage != before
    }

    private fun readConfirmation(text: String): Boolean {
        val k = key ?: return false
        val expectedRole = if (mode is Mode.Join) "L" else if (mode is Mode.Invite) "J" else return false
        if (!MessageDigest.isEqual(text.toByteArray(Charsets.UTF_8), proof(expectedRole, k).toByteArray(Charsets.UTF_8))) return false
        if (mode is Mode.Join) {
            if (leaderConfirmed) return false
            leaderConfirmed = true
            if (localConfirmed) qr = proof("J", k)
        } else {
            if (!localConfirmed || !peerComplete || stage == Stage.HandingOver) return false
            qr = TeamInvite.encode((mode as Mode.Invite).team, k)
            stage = Stage.HandingOver
        }
        return true
    }

    private fun readTeamCode(text: String): Boolean {
        val k = key ?: return false
        when (mode) {
            is Mode.Invite -> {
                if (!localConfirmed || stage != Stage.HandingOver) return false
                val name = TeamInvite.readAck(text, mode.team, k) ?: return false
                memberName = name
                stage = Stage.Done
                return true
            }
            Mode.Join -> {
                if (!localConfirmed || !leaderConfirmed) return false
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

    companion object {
        const val TEAM_PAIR_PREFIX = "FFTPAIR2:"
        const val CONFIRM_PREFIX = "FFTCONF2:"
    }
}
