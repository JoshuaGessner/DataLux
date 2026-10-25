package dev.fileflow.secure

import java.io.ByteArrayOutputStream
import java.nio.BufferUnderflowException
import java.nio.ByteBuffer
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/** Someone in a team, by the name they gave their phone (Settings › Teams › Your name). */
class Member(
    val name: String,
    val joinedMs: Long,
    val leader: Boolean = false,
    /** This phone's own entry. */
    val you: Boolean = false,
    /** Was in the team before its key was changed, and must be invited again to hold the new one. */
    val needsNewKey: Boolean = false,
)

/**
 * A team: one AES-256 key that everyone in it holds, so one transfer -- one screen, read by
 * several cameras at once -- opens on every member's phone and on no one else's.
 *
 * A pairing joins two phones for half an hour; a team is kept, because the point of one is a
 * group that sends to itself again and again. Its key reaches a new member only through a fresh
 * pairing ([TeamInvite]): face to face, sealed under that pairing's key, never on a screen in the
 * clear.
 *
 * The phone that made a team LEADS it: it invites, and it keeps the list of [members] -- each new
 * member's name comes back to it as they join. A member's list is the team as it stood when they
 * joined. Holding the key is what membership IS, and a key cannot be taken back from a phone, so
 * removing someone means a new key ([rotated]) that everyone else is invited to again.
 */
class Team(
    override val id: ByteArray,
    val name: String,
    override val secret: SecretKeySpec,
    val createdMs: Long,
    val isLeader: Boolean = true,
    /** Leader first. Includes this phone ([Member.you]). */
    val members: List<Member> = emptyList(),
) : SealKey {
    override val label: String get() = "team $name"

    /** The id as hex, to name the team in an intent or a preference. */
    val idHex: String get() = id.joinToString("") { "%02x".format(it) }

    val leader: Member? get() = members.firstOrNull { it.leader }

    /** With [m] added, or in place of an entry of the same name that was waiting for the new key. */
    fun withMember(m: Member): Team {
        val rest = members.filterNot { !it.leader && !it.you && it.name == m.name && it.needsNewKey }
        return copy(members = rest + m)
    }

    fun without(member: Member): Team = copy(members = members.filterNot { it === member })

    fun renamed(newName: String): Team = copy(name = cleanName(newName).ifEmpty { name })

    /**
     * The same team under a new key and id: what removing someone takes. Everyone else is kept on
     * the list, marked [Member.needsNewKey] until they are invited again; files already sent stay
     * readable to whoever holds the old key.
     */
    fun rotated(random: SecureRandom = SecureRandom()): Team = Team(
        ByteArray(PairingKey.ID_BYTES).also(random::nextBytes),
        name,
        SecretKeySpec(ByteArray(KEY_BYTES).also(random::nextBytes), "AES"),
        createdMs,
        isLeader,
        members.map { if (it.leader || it.you) it else Member(it.name, it.joinedMs, needsNewKey = true) },
    )

    private fun copy(name: String = this.name, members: List<Member> = this.members) =
        Team(id, name, secret, createdMs, isLeader, members)

    companion object {
        const val MAX_NAME_CHARS = 40
        internal const val KEY_BYTES = 32

        /** A new team, led by this phone, whose person is [myName]. */
        fun create(
            name: String, myName: String = "", nowMs: Long = System.currentTimeMillis(),
            random: SecureRandom = SecureRandom(),
        ): Team {
            val clean = cleanName(name)
            require(clean.isNotEmpty()) { "a team needs a name" }
            return Team(
                ByteArray(PairingKey.ID_BYTES).also(random::nextBytes),
                clean,
                SecretKeySpec(ByteArray(KEY_BYTES).also(random::nextBytes), "AES"),
                nowMs,
                isLeader = true,
                members = listOf(Member(cleanName(myName), nowMs, leader = true, you = true)),
            )
        }

        fun cleanName(name: String): String =
            name.filter { !it.isISOControl() }.trim().take(MAX_NAME_CHARS)

        internal fun fromParts(
            id: ByteArray, name: String, key: ByteArray, createdMs: Long,
            isLeader: Boolean = true, members: List<Member> = emptyList(),
        ): Team? {
            if (id.size != PairingKey.ID_BYTES || key.size != KEY_BYTES) return null
            val clean = cleanName(name).ifEmpty { return null }
            return Team(id, clean, SecretKeySpec(key, "AES"), createdMs, isLeader, members)
        }
    }
}

/** Short strings and lists in the byte formats below: a length byte, then UTF-8. */
internal object Wire {
    fun str(out: ByteArrayOutputStream, s: String) {
        val b = s.toByteArray(Charsets.UTF_8).let { if (it.size <= 255) it else it.copyOf(255) }
        out.write(b.size)
        out.write(b)
    }

    fun long(out: ByteArrayOutputStream, v: Long) = out.write(ByteBuffer.allocate(8).putLong(v).array())

    fun str(buf: ByteBuffer): String = ByteArray(buf.get().toInt() and 0xFF).also { buf.get(it) }.toString(Charsets.UTF_8)

    /** Members as `count (u16) | count x [ name | joined ms (u64) | flags (u8) ]`. */
    fun members(out: ByteArrayOutputStream, list: List<Member>) {
        out.write(list.size shr 8)
        out.write(list.size and 0xFF)
        list.forEach {
            str(out, it.name)
            long(out, it.joinedMs)
            out.write((if (it.leader) 1 else 0) or (if (it.you) 2 else 0) or (if (it.needsNewKey) 4 else 0))
        }
    }

    fun members(buf: ByteBuffer): List<Member> = List(buf.short.toInt() and 0xFFFF) {
        val name = Team.cleanName(str(buf))
        val joined = buf.long
        val f = buf.get().toInt()
        Member(name, joined, leader = f and 1 != 0, you = f and 2 != 0, needsNewKey = f and 4 != 0)
    }
}

/**
 * Handing a team's key to a new member, over a pairing made for the purpose.
 *
 * The two phones pair face to face as usual ([Pairing]); the leader's screen then shows the team --
 * id, key, name, who leads it and who is in it -- sealed with AES-256-GCM under the new pairing's
 * key. Only the phone that just paired with it can open that code, and a camera across the room that
 * reads it learns nothing. The member answers with its person's name and a short proof that only a
 * holder of the team key can make, sealed the same way: that is how the leader's screen knows to say
 * who joined, and how the leader's list of members grows.
 *
 * ```
 * invite = "FFTEAM2:" + b64( nonce (12) | GCM(pairing key;
 *              id (8) | key (32) | created ms (u64) | team name | leader's name | members) )
 * ack    = "FFTEAMOK2:" + b64( nonce (12) | GCM(pairing key;
 *              HMAC-SHA256(team key, "FileFlow team joined" | pairing id)[0..8] | member's name) )
 *          associated data: the prefix | pairing id
 * ```
 * Names are a length byte and UTF-8; members as [Wire.members], without their flags' meaning to the
 * sender (the receiver marks its own entry).
 */
object TeamInvite {
    const val PREFIX = "FFTEAM2:"
    const val ACK_PREFIX = "FFTEAMOK2:"
    private const val NONCE_BYTES = 12
    private const val TAG_BITS = 128
    private const val PROOF_BYTES = 8

    /** Most members an invite lists: the code must stay small enough for a front camera to read. */
    const val MAX_LISTED = 12

    /** True for any team code -- this version's or another -- so it is never taken for a pairing code. */
    fun isTeamCode(text: String) = text.startsWith("FFTEAM")

    fun encode(team: Team, pairing: PairingKey, random: SecureRandom = SecureRandom()): String {
        val out = ByteArrayOutputStream()
        out.write(team.id)
        out.write(team.secret.encoded)
        Wire.long(out, team.createdMs)
        Wire.str(out, team.name)
        Wire.str(out, team.leader?.name.orEmpty())
        // Who holds THIS key already, newest last; not the leader (said above) nor anyone waiting
        // to be invited again.
        Wire.members(out, team.members.filter { !it.leader && !it.needsNewKey }.takeLast(MAX_LISTED)
            .map { Member(it.name, it.joinedMs) })
        return PREFIX + b64(seal(out.toByteArray(), pairing, PREFIX, random))
    }

    /**
     * The team in [text], as the joining phone keeps it -- led by the sender, this phone [myName]
     * among its members -- or null when it is not an invite, or not sealed to [pairing].
     */
    fun decode(text: String, pairing: PairingKey, myName: String, nowMs: Long = System.currentTimeMillis()): Team? {
        if (!text.startsWith(PREFIX)) return null
        val plain = open(text.removePrefix(PREFIX), pairing, PREFIX) ?: return null
        return try {
            val buf = ByteBuffer.wrap(plain)
            val id = ByteArray(PairingKey.ID_BYTES).also { buf.get(it) }
            val key = ByteArray(Team.KEY_BYTES).also { buf.get(it) }
            val created = buf.long
            val name = Wire.str(buf)
            val leader = Team.cleanName(Wire.str(buf))
            val others = Wire.members(buf).map { Member(it.name, it.joinedMs) }
            if (buf.hasRemaining()) return null
            Team.fromParts(id, name, key, created, isLeader = false,
                members = listOf(Member(leader, created, leader = true)) + others +
                    Member(Team.cleanName(myName), nowMs, you = true))
        } catch (_: BufferUnderflowException) {
            null
        }
    }

    /** What a member shows once it holds [team]: its name, with proof that it holds the key. */
    fun ack(team: Team, pairing: PairingKey, myName: String, random: SecureRandom = SecureRandom()): String {
        val out = ByteArrayOutputStream()
        out.write(proof(team, pairing))
        Wire.str(out, Team.cleanName(myName))
        return ACK_PREFIX + b64(seal(out.toByteArray(), pairing, ACK_PREFIX, random))
    }

    /** The new member's name, when [text] is their acknowledgement of [team]; null otherwise. */
    fun readAck(text: String, team: Team, pairing: PairingKey): String? {
        if (!text.startsWith(ACK_PREFIX)) return null
        val plain = open(text.removePrefix(ACK_PREFIX), pairing, ACK_PREFIX) ?: return null
        if (plain.size < PROOF_BYTES + 1) return null
        if (!java.security.MessageDigest.isEqual(plain.copyOf(PROOF_BYTES), proof(team, pairing))) return null
        return try {
            val buf = ByteBuffer.wrap(plain, PROOF_BYTES, plain.size - PROOF_BYTES)
            Team.cleanName(Wire.str(buf)).takeIf { !buf.hasRemaining() }
        } catch (_: BufferUnderflowException) {
            null
        }
    }

    private fun proof(team: Team, pairing: PairingKey): ByteArray =
        Mac.getInstance("HmacSHA256").run {
            init(SecretKeySpec(team.secret.encoded, "HmacSHA256"))
            doFinal("FileFlow team joined".toByteArray(Charsets.US_ASCII) + pairing.id)
        }.copyOf(PROOF_BYTES)

    private fun seal(plain: ByteArray, pairing: PairingKey, prefix: String, random: SecureRandom): ByteArray {
        val nonce = ByteArray(NONCE_BYTES).also(random::nextBytes)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, pairing.secret, GCMParameterSpec(TAG_BITS, nonce))
        cipher.updateAAD(prefix.toByteArray(Charsets.US_ASCII) + pairing.id)
        return nonce + cipher.doFinal(plain)
    }

    private fun open(body: String, pairing: PairingKey, prefix: String): ByteArray? {
        val bytes = runCatching { unb64(body) }.getOrNull() ?: return null
        if (bytes.size < NONCE_BYTES + TAG_BITS / 8) return null
        return try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, pairing.secret, GCMParameterSpec(TAG_BITS, bytes, 0, NONCE_BYTES))
            cipher.updateAAD(prefix.toByteArray(Charsets.US_ASCII) + pairing.id)
            cipher.doFinal(bytes, NONCE_BYTES, bytes.size - NONCE_BYTES)
        } catch (_: java.security.GeneralSecurityException) {
            null
        }
    }

    private fun b64(b: ByteArray): String = java.util.Base64.getUrlEncoder().withoutPadding().encodeToString(b)

    private fun unb64(s: String): ByteArray = java.util.Base64.getUrlDecoder().decode(s)
}

/**
 * The teams this phone is in, kept between runs. Each key is stored wrapped by a key that never
 * leaves the phone's secure hardware ([KeyWrap]; on Android, the Keystore), so the saved file alone
 * opens nothing -- not a backup, not a copy pulled off the phone.
 */
class TeamStore(private val storage: Storage, private val wrap: KeyWrap) {

    /** Where the list lives: one string, rewritten whole. */
    interface Storage {
        fun load(): String?
        fun save(s: String)
    }

    interface KeyWrap {
        fun wrap(key: ByteArray): ByteArray
        fun unwrap(wrapped: ByteArray): ByteArray
    }

    private var teams: List<Team>? = null

    @Synchronized
    fun all(): List<Team> = teams ?: load().also { teams = it }

    fun find(idHex: String?): Team? = idHex?.let { h -> all().firstOrNull { it.idHex == h } }

    /** Adds [team], replacing one with the same id (joining twice is harmless). */
    @Synchronized
    fun add(team: Team) {
        save(all().filterNot { it.id.contentEquals(team.id) } + team)
    }

    /** Puts [team] where the team [oldIdHex] was -- the same team after a change of key. */
    @Synchronized
    fun replace(oldIdHex: String, team: Team) {
        val list = all()
        val i = list.indexOfFirst { it.idHex == oldIdHex }
        save(if (i < 0) list + team else list.toMutableList().also { it[i] = team })
    }

    @Synchronized
    fun remove(idHex: String) {
        save(all().filterNot { it.idHex == idHex })
    }

    // One line per team: id, name, wrapped key (base64), created ms, then -- since teams have members
    // -- "L" or "M" (leader or member) and the member list (base64 of Wire.members). A line with only
    // the first four is a team from before: led here, its list empty. A line that will not open --
    // the wrapping key gone after a restore to a new phone -- is dropped, not fatal: that team is
    // simply joined again.
    private fun load(): List<Team> = storage.load().orEmpty().lines().mapNotNull { line ->
        val f = line.split('\t')
        if (f.size != 4 && f.size != 6) return@mapNotNull null
        runCatching {
            val d = java.util.Base64.getDecoder()
            val members = if (f.size == 6) Wire.members(ByteBuffer.wrap(d.decode(f[5]))) else emptyList()
            Team.fromParts(d.decode(f[0]), d.decode(f[1]).toString(Charsets.UTF_8),
                           wrap.unwrap(d.decode(f[2])), f[3].toLong(),
                           isLeader = f.size == 4 || f[4] == "L", members = members)
        }.getOrNull()
    }

    private fun save(list: List<Team>) {
        val e = java.util.Base64.getEncoder()
        storage.save(list.joinToString("\n") {
            val roster = ByteArrayOutputStream().also { o -> Wire.members(o, it.members) }.toByteArray()
            listOf(e.encodeToString(it.id), e.encodeToString(it.name.toByteArray(Charsets.UTF_8)),
                   e.encodeToString(wrap.wrap(it.secret.encoded)), it.createdMs.toString(),
                   if (it.isLeader) "L" else "M", e.encodeToString(roster)).joinToString("\t")
        })
        teams = list
    }
}
