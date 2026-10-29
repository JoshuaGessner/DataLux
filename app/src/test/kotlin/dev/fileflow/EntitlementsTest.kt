package dev.fileflow

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class EntitlementsTest {
    @Test fun anUnsignedPurchaseFlagCannotUnlockAndroidPro() {
        Entitlements.useVerifiedLicense("claimed-pro-without-signature", "account")
        val expected = !BuildConfig.STORE
        assertEquals(expected, Entitlements.has(Entitlements.Feature.Batch))
        assertEquals(expected, Entitlements.has(Entitlements.Feature.BatchReceive))
        assertEquals(expected, Entitlements.has(Entitlements.Feature.BulkManage))
    }

    @Test fun clearingActivationKeepsSecurityAndTeamsFree() {
        Entitlements.useVerifiedLicense(null, "account")
        assertTrue(Entitlements.has(Entitlements.Feature.Teams))
        assertEquals(if (BuildConfig.STORE) Entitlements.Tier.Free else Entitlements.Tier.Pro, Entitlements.tier)
    }
}
