package dev.fileflow

import org.junit.Assert.assertEquals
import org.junit.Test

class SendBrightnessTest {
    @Test fun preferenceValuesAreStableAndInvalidValuesUseTheExistingFullBrightness() {
        assertEquals(SendBrightness.Full, SendBrightness.named(null))
        assertEquals(SendBrightness.Full, SendBrightness.named("unknown"))
        assertEquals(0.4f, SendBrightness.named("Low").level, 0f)
        assertEquals(0.65f, SendBrightness.named("Medium").level, 0f)
        assertEquals(1f, SendBrightness.named("Full").level, 0f)
    }
}
