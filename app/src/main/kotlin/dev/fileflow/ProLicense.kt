package dev.fileflow

import java.security.KeyFactory
import java.security.MessageDigest
import java.security.Signature
import java.security.interfaces.ECPublicKey
import java.security.spec.X509EncodedKeySpec
import java.util.Base64

/** The trust anchor is compiled into the app, never downloaded alongside a license. */
object ProLicense {
    private val uuid = Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
    private val encoded = Regex("[A-Za-z0-9_-]+")

    fun verify(license: String, publicKeyBase64: String, expectedUserId: String): Boolean = runCatching {
        require(license.length in 1..4096 && publicKeyBase64.length in 1..1024)
        require(uuid.matches(expectedUserId))
        val envelope = license.split('\n')
        require(envelope.size == 5 && envelope[0] == "FILEFLOW-PRO-1" && envelope[4].isEmpty())
        require(envelope[1].matches(Regex("[0-9a-f]{16}")))
        require(encoded.matches(envelope[2]) && encoded.matches(envelope[3]))
        val der = Base64.getDecoder().decode(publicKeyBase64)
        val key = KeyFactory.getInstance("EC").generatePublic(X509EncodedKeySpec(der)) as ECPublicKey
        // SPKI must be a P-256 key; the backend uses the same named curve.
        require(key.params.curve.field.fieldSize == 256 && key.params.order.bitLength() == 256)
        val kid = MessageDigest.getInstance("SHA-256").digest(der).take(8)
            .joinToString("") { "%02x".format(it.toInt() and 255) }
        require(envelope[1] == kid)
        val payload = Base64.getUrlDecoder().decode(envelope[2])
        val signature = Base64.getUrlDecoder().decode(envelope[3])
        // Require canonical base64url so alternate encodings cannot bypass format comparisons.
        require(Base64.getUrlEncoder().withoutPadding().encodeToString(payload) == envelope[2])
        require(Base64.getUrlEncoder().withoutPadding().encodeToString(signature) == envelope[3])
        require(payload.all { it.toInt() in 10..126 && (it.toInt() == 10 || it.toInt() >= 32) })
        val claims = payload.toString(Charsets.US_ASCII).split('\n')
        require(claims.size == 7 && claims[0] == "fileflow-pro-v1" && claims[1] == expectedUserId)
        require(uuid.matches(claims[2]) && claims[3].matches(Regex("[1-9][0-9]{0,15}")))
        require(claims[3].toLong() > 0 && claims[4] == "perpetual" && claims[5] == "batch,bulk_manage" && claims[6].isEmpty())
        Signature.getInstance("SHA256withECDSA").run {
            initVerify(key)
            update(payload)
            verify(signature)
        }
    }.getOrDefault(false)
}
