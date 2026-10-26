package dev.fileflow

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.Signature
import java.security.spec.ECGenParameterSpec
import java.util.Base64

class ProLicenseTest {
    private val pair = KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1")) }.generateKeyPair()
    private val user = "19444c5e-5a94-490a-a6c3-352a461cf677"
    private val publicKey = Base64.getEncoder().encodeToString(pair.public.encoded)
    private val payload = "fileflow-pro-v1\n$user\n58d0f688-64c1-46e4-8cc2-106a581e781b\n1792890000000\nperpetual\nbatch,bulk_manage\n"

    private fun sign(claims: String): String {
        val raw = claims.toByteArray(Charsets.UTF_8)
        val sig = Signature.getInstance("SHA256withECDSA").run { initSign(pair.private); update(raw); sign() }
        val kid = MessageDigest.getInstance("SHA-256").digest(pair.public.encoded).take(8).joinToString("") { "%02x".format(it.toInt() and 255) }
        val encode = Base64.getUrlEncoder().withoutPadding()
        return "FILEFLOW-PRO-1\n$kid\n${encode.encodeToString(raw)}\n${encode.encodeToString(sig)}\n"
    }

    @Test fun verifiesPerpetualLicenseForItsAccount() {
        assertTrue(ProLicense.verify(sign(payload), publicKey, user))
    }
    @Test fun rejectsWrongAccountOrTrustAnchor() {
        assertFalse(ProLicense.verify(sign(payload), publicKey, "29444c5e-5a94-490a-a6c3-352a461cf677"))
        assertFalse(ProLicense.verify(sign(payload), "", user))
        val other = KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1")) }.generateKeyPair()
        assertFalse(ProLicense.verify(sign(payload), Base64.getEncoder().encodeToString(other.public.encoded), user))
    }
    @Test fun rejectsUnsignedAndMalformedClaims() {
        val valid = sign(payload)
        val envelope = valid.split('\n').toMutableList()
        envelope[2] = Base64.getUrlEncoder().withoutPadding().encodeToString(payload.replace("batch,bulk_manage", "all").toByteArray())
        assertFalse(ProLicense.verify(envelope.joinToString("\n"), publicKey, user))
        assertFalse(ProLicense.verify(sign(payload.replace("perpetual", "trial")), publicKey, user))
        assertFalse(ProLicense.verify(sign(payload.replace("1792890000000", "-1")), publicKey, user))
        assertFalse(ProLicense.verify(valid + "extra\n", publicKey, user))
        assertFalse(ProLicense.verify(valid.repeat(100), publicKey, user))
    }
}
