package dev.fileflow.secure

import java.nio.ByteBuffer
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * A team: one AES-256 key that everyone in it holds, so one transfer -- one screen, read by
 * several cameras at once -- opens on every member's phone and on no one else's.
 *
 * A pairing joins two phones for half an hour; a team is kept, because the point of one is a
 * group that sends to itself again and again. Its key reaches a new member only through a fresh
 * pairing ([TeamInvite]): face to face, sealed under that pairing's key, never on a screen in the
 * clear.
 *
 * There is no list of members: holding the key is membership. Removing someone means making a new
 * team and inviting everyone else to it.
 */
class Team(
    override val id: ByteArray,
    val name: String,
    override val secret: SecretKeySpec,
    val createdMs: Long,
) : SealKey {
    override val label: String get() = "team $name"

    /** The id as hex, to name the team in an intent or a preference. */
    val idHex: String get() = id.joinToString("") { "%02x".format(it) }

    companion object {
        const val MAX_NAME_CHARS = 40
        private const val KEY_BYTES = 32

        fun create(name: String, nowMs: Long = System.currentTimeMillis(), random: SecureRandom = SecureRandom()): Team {
            val clean = cleanName(name)
            require(clean.isNotEmpty()) { "a team needs a name" }
            return Team(
                ByteArray(PairingKey.ID_BYTES).also(random::nextBytes),
                clean,
                SecretKeySpec(ByteArray(KEY_BYTES).also(random::nextBytes), "AES"),
                nowMs,
            )
        }

        fun cleanName(name: String): String =
            name.filter { !it.isISOControl() }.trim().take(MAX_NAME_CHARS)

        internal fun fromParts(id: ByteArray, name: String, key: ByteArray, createdMs: Long): Team? {
            if (id.size != PairingKey.ID_BYTES || key.size != KEY_BYTES) return null
            val clean = cleanName(name).ifEmpty { return null }
            return Team(id, clean, SecretKeySpec(key, "AES"), createdMs)
        }
    }
}

/**
 * Handing a team's key to a new member, over a pairing made for the purpose.
 *
 * The two phones pair face to face as usual ([Pairing]); the leader's screen then shows the team
 * -- id, key, name -- sealed with AES-256-GCM under the new pairing's key. Only the phone that
 * just paired with it can open that code, and a camera across the room that reads it learns
 * nothing. The member answers with a short code only a holder of the team key can make, which is
 * how the leader's screen knows to say "joined".
 *
 * ```
 * invite = "FFTEAM1:" + b64( nonce (12) | GCM(pairing key; id (8) | key (32) | created ms (u64) | name) )
 *          associated data: "FFTEAM1:" | pairing id
 * ack    = "FFTEAMOK:" + b64( HMAC-SHA256(team key, "FileFlow team joined" | pairing id)[0..8] )
 * ```
 */
object TeamInvite {
    const val PREFIX = "FFTEAM1:"
    const val ACK_PREFIX = "FFTEAMOK:"
    private const val NONCE_BYTES = 12
    private const val TAG_BITS = 128
    private const val ACK_BYTES = 8

    fun encode(team: Team, pairing: PairingKey, random: SecureRandom = SecureRandom()): String {
        val name = team.name.toByteArray(Charsets.UTF_8)
        val plain = ByteBuffer.allocate(PairingKey.ID_BYTES + team.secret.encoded.size + 8 + name.size)
            .put(team.id).put(team.secret.encoded).putLong(team.createdMs).put(name).array()
        val nonce = ByteArray(NONCE_BYTES).also(random::nextBytes)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, pairing.secret, GCMParameterSpec(TAG_BITS, nonce))
        cipher.updateAAD(aad(pairing))
        return PREFIX + b64(nonce + cipher.doFinal(plain))
    }

    /** The team in [text], or null when it is not an invite, or not sealed to [pairing]. */
    fun decode(text: String, pairing: PairingKey): Team? {
        if (!text.startsWith(PREFIX)) return null
        val bytes = runCatching { unb64(text.removePrefix(PREFIX)) }.getOrNull() ?: return null
        if (bytes.size < NONCE_BYTES + TAG_BITS / 8) return null
        val plain = try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, pairing.secret, GCMParameterSpec(TAG_BITS, bytes, 0, NONCE_BYTES))
            cipher.updateAAD(aad(pairing))
            cipher.doFinal(bytes, NONCE_BYTES, bytes.size - NONCE_BYTES)
        } catch (_: java.security.GeneralSecurityException) {
            return null
        }
        if (plain.size < PairingKey.ID_BYTES + 32 + 8) return null
        val buf = ByteBuffer.wrap(plain)
        val id = ByteArray(PairingKey.ID_BYTES).also { buf.get(it) }
        val key = ByteArray(32).also { buf.get(it) }
        val created = buf.long
        val name = ByteArray(buf.remaining()).also { buf.get(it) }.toString(Charsets.UTF_8)
        return Team.fromParts(id, name, key, created)
    }

    /** What a member shows once it holds [team]: proof of that, for the leader's screen. */
    fun ack(team: Team, pairing: PairingKey): String = ACK_PREFIX + b64(ackBytes(team, pairing))

    fun isAck(text: String, team: Team, pairing: PairingKey): Boolean {
        if (!text.startsWith(ACK_PREFIX)) return false
        val got = runCatching { unb64(text.removePrefix(ACK_PREFIX)) }.getOrNull() ?: return false
        return java.security.MessageDigest.isEqual(got, ackBytes(team, pairing))
    }

    private fun ackBytes(team: Team, pairing: PairingKey): ByteArray =
        Mac.getInstance("HmacSHA256").run {
            init(SecretKeySpec(team.secret.encoded, "HmacSHA256"))
            doFinal("FileFlow team joined".toByteArray(Charsets.US_ASCII) + pairing.id)
        }.copyOf(ACK_BYTES)

    private fun aad(pairing: PairingKey) = PREFIX.toByteArray(Charsets.US_ASCII) + pairing.id

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

    @Synchronized
    fun remove(idHex: String) {
        save(all().filterNot { it.idHex == idHex })
    }

    // One line per team: id, name, wrapped key (base64), created ms. A line that will not open --
    // the wrapping key gone after a restore to a new phone -- is dropped, not fatal: that team
    // is simply joined again.
    private fun load(): List<Team> = storage.load().orEmpty().lines().mapNotNull { line ->
        val f = line.split('\t')
        if (f.size != 4) return@mapNotNull null
        runCatching {
            val d = java.util.Base64.getDecoder()
            Team.fromParts(d.decode(f[0]), d.decode(f[1]).toString(Charsets.UTF_8),
                           wrap.unwrap(d.decode(f[2])), f[3].toLong())
        }.getOrNull()
    }

    private fun save(list: List<Team>) {
        val e = java.util.Base64.getEncoder()
        storage.save(list.joinToString("\n") {
            listOf(e.encodeToString(it.id), e.encodeToString(it.name.toByteArray(Charsets.UTF_8)),
                   e.encodeToString(wrap.wrap(it.secret.encoded)), it.createdMs.toString()).joinToString("\t")
        })
        teams = list
    }
}
