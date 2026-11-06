package dev.fileflow

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ProVerificationStateTest {
    @Test fun absentOrFailedRefreshDoesNotClaimEmailIsUnverified() {
        assertEquals(VerificationUiState.CHECKING, verificationUiState(null, refreshing = true, failed = false))
        assertEquals(VerificationUiState.UNKNOWN, verificationUiState(null, refreshing = false, failed = true))
        assertEquals(VerificationUiState.UNKNOWN, verificationUiState(false, refreshing = false, failed = true))
        assertEquals(VerificationUiState.UNKNOWN, verificationUiState(true, refreshing = false, failed = true))
        assertEquals(VerificationUiState.CHECKING, verificationUiState(false, refreshing = true, failed = true))
        assertEquals(VerificationUiState.UNVERIFIED, verificationUiState(false, refreshing = false, failed = false))
        assertEquals(VerificationUiState.VERIFIED, verificationUiState(true, refreshing = false, failed = false))
    }

    @Test fun checkoutRequiresCurrentVerifiedAndEnabledAccountStatus() {
        val verified = ProStatus(false, true, CURRENT_PRO_PRODUCT, "id", true, true)
        assertFalse(checkoutAccountReady(null, refreshFailed = false))
        assertFalse(checkoutAccountReady(verified, refreshFailed = true))
        assertFalse(checkoutAccountReady(verified.copy(emailVerified = false), refreshFailed = false))
        assertFalse(checkoutAccountReady(verified.copy(billingEnabled = false), refreshFailed = false))
        assertTrue(checkoutAccountReady(verified, refreshFailed = false))
    }

    @Test fun newProductRequiresExplicitServerCapability() {
        assertFalse(canSellCurrentPro(true, null))
        assertFalse(canSellCurrentPro(true, LEGACY_PRO_PRODUCT))
        assertFalse(canSellCurrentPro(false, CURRENT_PRO_PRODUCT))
        assertTrue(canSellCurrentPro(true, CURRENT_PRO_PRODUCT))
    }

    @Test fun partialLoginIsDistinctFromBadCredentialsOrExpiredSession() {
        val previous = ProSession("old", "user", "user@example.invalid", Long.MAX_VALUE)
        val newSession = previous.copy(token = "new")
        assertFalse(loginSavedSessionBeforeFailure(null, null, requiresSignIn = true))
        assertFalse(loginSavedSessionBeforeFailure(previous, previous, requiresSignIn = true))
        assertFalse(loginSavedSessionBeforeFailure(previous, newSession, requiresSignIn = true))
        assertTrue(loginSavedSessionBeforeFailure(previous, newSession, requiresSignIn = false))
        assertTrue(loginSavedSessionBeforeFailure(null, newSession, requiresSignIn = false))
    }
}
