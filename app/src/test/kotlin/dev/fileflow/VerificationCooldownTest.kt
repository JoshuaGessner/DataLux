package dev.fileflow

import org.junit.Assert.assertEquals
import org.junit.Test

class VerificationCooldownTest {
    @Test fun countdownDoesNotEnableResendBeforeTheDeadline() {
        assertEquals(60L, VerificationCooldown.secondsRemaining(61_000, 1_001))
        assertEquals(1L, VerificationCooldown.secondsRemaining(61_000, 60_999))
        assertEquals(0L, VerificationCooldown.secondsRemaining(61_000, 61_000))
        assertEquals(0L, VerificationCooldown.secondsRemaining(61_000, 70_000))
    }

    @Test fun oldServersAndBadRetryHeadersUseABoundedRetryWindow() {
        assertEquals(61_000L, VerificationCooldown.retryDeadline(null, 1_000))
        assertEquals(61_000L, VerificationCooldown.retryDeadline("invalid", 1_000))
        assertEquals(121_000L, VerificationCooldown.retryDeadline("120", 1_000))
        assertEquals(3_601_000L, VerificationCooldown.retryDeadline("99999999", 1_000))
        assertEquals(2_000L, VerificationCooldown.retryDeadline("-1", 1_000))
    }

    @Test fun expiredAndExtremeServerDeadlinesDoNotFreezeTheUi() {
        assertEquals(0L, VerificationCooldown.serverDeadline(0, 1_000))
        assertEquals(0L, VerificationCooldown.serverDeadline(999, 1_000))
        assertEquals(61_000L, VerificationCooldown.serverDeadline(61_000, 1_000))
        assertEquals(3_601_000L, VerificationCooldown.serverDeadline(Long.MAX_VALUE, 1_000))
        assertEquals(3_600L, VerificationCooldown.secondsRemaining(Long.MAX_VALUE, 1_000))
    }
}
