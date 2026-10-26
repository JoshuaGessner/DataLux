package dev.fileflow

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class AccountErrorTest {
    @Test fun verificationErrorsDoNotLookLikePurchaseOwnershipErrors() {
        val error = AccountError.fromResponse(403, "EMAIL_NOT_VERIFIED", true)
        assertTrue(error.message!!.contains("Verify your email"))
        assertFalse(error.message!!.contains("different FileFlow account"))
        assertFalse(error.invalidatesAuthentication)
    }

    @Test fun wrongDeletionPasswordKeepsTheSignedInAccount() {
        val error = AccountError.fromResponse(401, "INVALID_PASSWORD", true, deleting = true)
        assertTrue(error.message!!.contains("not been deleted"))
        assertFalse(error.invalidatesAuthentication)
    }

    @Test fun expiredSessionRequestsSignInButDoesNotClaimOfflineAccessWasRemoved() {
        val error = AccountError.fromResponse(401, "", true)
        assertTrue(error.invalidatesAuthentication)
        assertTrue(error.message!!.contains("offline Pro access has been kept"))
    }

    @Test fun deletionAndVerificationOutagesDoNotPromiseSuccessOrEraseAccess() {
        val deletion = AccountError.fromResponse(503, "", true, deleting = true)
        assertTrue(deletion.message!!.contains("unavailable"))
        assertTrue(deletion.message!!.contains("unchanged"))
        assertFalse(deletion.invalidatesAuthentication)
        val verification = AccountError.fromResponse(503, "MAIL_NOT_CONFIGURED", true, verifying = true)
        assertTrue(verification.message!!.contains("Verification email"))
        assertFalse(verification.invalidatesAuthentication)
    }
}
