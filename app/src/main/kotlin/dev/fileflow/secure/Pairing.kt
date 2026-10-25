package dev.fileflow.secure

import java.math.BigInteger
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * The key two paired phones share: an AES-256 key, a short id that names it inside each envelope,
 * and the six-digit code both phones show so the people holding them can check they match.
 */
class PairingKey(
    override val secret: SecretKeySpec,
    override val id: ByteArray,
    /** "482 913": the same on both phones, or someone else's key was scanned. */
    val code: String,
    val createdMs: Long,
) : SealKey {
    override val label: String get() = "paired phone"

    companion object {
        const val ID_BYTES = 8
    }
}

/**
 * Pairing by scanning, after Wi-Fi Easy Connect's QR bootstrapping and "Seeing-is-Believing"
 * (McCune, Perrig and Reiter, 2005): each phone shows a QR code holding a fresh public key, and
 * reads the other's with its camera, the two held face to face. A camera a few centimetres from a
 * screen is a channel nobody else can write to unnoticed, so the keys it carries are authentic
 * without any server, account or password.
 *
 *  - Keys are ephemeral P-256 pairs made for this pairing and never stored, so a later compromise
 *    of a phone reveals nothing about transfers already made (forward secrecy).
 *  - The shared secret is ECDH, run through HKDF-SHA256 with BOTH public keys as salt, so the key
 *    is bound to exactly this pair of phones.
 *  - Each phone also shows a six-digit code from the same derivation, as Signal's safety numbers
 *    and Bluetooth's numeric comparison do. Matching codes prove each phone scanned the other and
 *    not a third screen.
 *  - Completion is mutual and automatic: once a phone has its peer's key, its QR code also carries
 *    a short hash of that key. A phone that reads its OWN key's hash in its peer's code knows the
 *    exchange is complete on both sides.
 */
class Pairing private constructor(private val pair: KeyPair) {

    /** This phone's public key, as the 65-byte uncompressed P-256 point. */
    val publicPoint: ByteArray = encodePoint(pair.public as ECPublicKey)

    private var peerPoint: ByteArray? = null

    /** What this phone's QR code says now: its key, and an acknowledgement once it has the peer's. */
    fun qrText(): String {
        val ack = peerPoint?.let { ":" + b64(hash(it).copyOf(ACK_BYTES)) } ?: ""
        return PREFIX + b64(publicPoint) + ack
    }

    /** The result of reading one QR code. */
    enum class Read { NotOurs, Own, PeerKey, PeerComplete }

    /**
     * Takes one decoded QR [text]. [Read.PeerComplete] means both phones now hold both keys and
     * [key] can be derived; [Read.PeerKey] that this phone has the peer's key but the peer has not
     * yet confirmed it has ours.
     */
    fun read(text: String): Read {
        if (!text.startsWith(PREFIX)) return Read.NotOurs
        val parts = text.removePrefix(PREFIX).split(':')
        val point = runCatching { unb64(parts[0]) }.getOrNull() ?: return Read.NotOurs
        if (point.size != POINT_BYTES || point[0] != 4.toByte()) return Read.NotOurs
        // Our own code reflected back -- a mirror, a window -- is not a peer.
        if (point.contentEquals(publicPoint)) return Read.Own
        val known = peerPoint
        if (known != null && !known.contentEquals(point)) return Read.NotOurs  // a third phone
        if (runCatching { decodePoint(point) }.isFailure) return Read.NotOurs
        peerPoint = point
        val acked = parts.size > 1 &&
            runCatching { unb64(parts[1]) }.getOrNull()
                ?.contentEquals(hash(publicPoint).copyOf(ACK_BYTES)) == true
        return if (acked) Read.PeerComplete else Read.PeerKey
    }

    /** The shared key, once the peer's key has been read. */
    fun key(nowMs: Long = System.currentTimeMillis()): PairingKey? {
        val peer = peerPoint ?: return null
        val agreement = KeyAgreement.getInstance("ECDH")
        agreement.init(pair.private)
        agreement.doPhase(decodePoint(peer), true)
        val shared = agreement.generateSecret()
        val (lo, hi) = if (compare(publicPoint, peer) <= 0) publicPoint to peer else peer to publicPoint
        val salt = hash(LABEL.toByteArray(Charsets.US_ASCII) + lo + hi)
        val prk = hmac(salt, shared)
        val okm = expand(prk, "FileFlow v1 keys".toByteArray(Charsets.US_ASCII), 32 + PairingKey.ID_BYTES + 4)
        shared.fill(0)
        val code = (BigInteger(1, okm.copyOfRange(40, 44)).toLong() % 1_000_000L)
            .toString().padStart(6, '0').let { "${it.substring(0, 3)} ${it.substring(3)}" }
        return PairingKey(
            SecretKeySpec(okm.copyOfRange(0, 32), "AES"),
            okm.copyOfRange(32, 32 + PairingKey.ID_BYTES),
            code,
            nowMs,
        )
    }

    companion object {
        const val PREFIX = "FFPAIR1:"
        private const val LABEL = "FileFlow pairing v1"
        private const val POINT_BYTES = 65
        private const val ACK_BYTES = 8

        /** A fresh key pair for one pairing. */
        fun start(): Pairing {
            val g = KeyPairGenerator.getInstance("EC")
            g.initialize(ECGenParameterSpec("secp256r1"))
            return Pairing(g.generateKeyPair())
        }

        // The curve's parameters, taken from a generated key: P-256 is the same everywhere, and
        // this avoids writing its constants out by hand.
        private val params by lazy {
            val g = KeyPairGenerator.getInstance("EC")
            g.initialize(ECGenParameterSpec("secp256r1"))
            (g.generateKeyPair().public as ECPublicKey).params
        }

        private fun encodePoint(k: ECPublicKey): ByteArray {
            fun fixed(v: BigInteger): ByteArray {
                val b = v.toByteArray()
                return when {
                    b.size == 32 -> b
                    b.size > 32 -> b.copyOfRange(b.size - 32, b.size)
                    else -> ByteArray(32 - b.size) + b
                }
            }
            return byteArrayOf(4) + fixed(k.w.affineX) + fixed(k.w.affineY)
        }

        /** Rebuilds a public key from its 65-byte point; throws if it is not on the curve. */
        private fun decodePoint(p: ByteArray): java.security.PublicKey {
            val x = BigInteger(1, p.copyOfRange(1, 33))
            val y = BigInteger(1, p.copyOfRange(33, 65))
            // On-curve check before use: an invalid point is the classic attack on ECDH.
            val prime = (params.curve.field as java.security.spec.ECFieldFp).p
            val lhs = y.modPow(BigInteger.valueOf(2), prime)
            val rhs = x.modPow(BigInteger.valueOf(3), prime)
                .add(params.curve.a.multiply(x)).add(params.curve.b).mod(prime)
            require(x < prime && y < prime && lhs == rhs) { "not a point on P-256" }
            return KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(ECPoint(x, y), params))
        }

        private fun hash(b: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(b)

        private fun hmac(key: ByteArray, data: ByteArray): ByteArray =
            Mac.getInstance("HmacSHA256").run { init(SecretKeySpec(key, "HmacSHA256")); doFinal(data) }

        /** HKDF-Expand (RFC 5869). */
        private fun expand(prk: ByteArray, info: ByteArray, length: Int): ByteArray {
            val out = java.io.ByteArrayOutputStream()
            var t = ByteArray(0)
            var i = 1
            while (out.size() < length) {
                t = hmac(prk, t + info + byteArrayOf(i.toByte()))
                out.write(t)
                i++
            }
            return out.toByteArray().copyOf(length)
        }

        private fun compare(a: ByteArray, b: ByteArray): Int {
            for (i in a.indices) {
                val d = (a[i].toInt() and 0xFF) - (b[i].toInt() and 0xFF)
                if (d != 0) return d
            }
            return 0
        }

        private fun b64(b: ByteArray): String =
            java.util.Base64.getUrlEncoder().withoutPadding().encodeToString(b)

        private fun unb64(s: String): ByteArray = java.util.Base64.getUrlDecoder().decode(s)
    }
}

/**
 * The current pairing, held in memory only: it dies with the app, and expires after
 * [LIFETIME_MS] regardless, so a phone left paired is not paired forever.
 */
object PairingStore {
    const val LIFETIME_MS = 30L * 60 * 1000

    @Volatile
    private var key: PairingKey? = null

    fun current(nowMs: Long = System.currentTimeMillis()): PairingKey? =
        key?.takeIf { nowMs - it.createdMs < LIFETIME_MS }

    fun set(k: PairingKey) {
        key = k
    }

    fun forget() {
        key = null
    }
}
