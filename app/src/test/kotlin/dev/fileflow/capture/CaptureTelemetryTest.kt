package dev.fileflow.capture

import org.junit.Assert.*
import org.junit.Test

class CaptureTelemetryTest {
    @Test fun realtimeTimingAndEmptyNotificationsAreIndependent() {
        val metrics = CaptureTelemetry(latestFrameOnly = true, timestampsAreRealtime = true)
        metrics.notification(true)
        metrics.callbackStarted(100, 80)
        metrics.callbackFinished(10)
        metrics.notification(false)
        metrics.notification(true)
        metrics.callbackStarted(150, 120)
        metrics.callbackFinished(30)
        val s = metrics.snapshot()
        assertTrue(s.latestFrameOnly)
        assertEquals(3L, s.imageNotifications)
        assertEquals(2L, s.acquiredImages)
        assertEquals(1L, s.emptyNotifications)
        assertEquals(2L, s.callbackCount)
        assertEquals(1L, s.callbackInterval.samples)
        assertEquals(50L, s.callbackInterval.maxNs)
        assertEquals(25.0, s.deliveryAge.meanNs, 0.0)
        assertEquals(20.0, s.callbackDuration.meanNs, 0.0)
        assertNull(s.supersededImages)
    }

    @Test fun unknownClockNeverClaimsAnAge() {
        val metrics = CaptureTelemetry(false, false)
        metrics.callbackStarted(100, 10)
        metrics.callbackFinished(0)
        assertEquals(0L, metrics.snapshot().deliveryAge.samples)
        assertEquals(0L, metrics.snapshot().invalidDeliveryTimestamps)
    }

    @Test fun invalidTimestampsAndClockRegressionDoNotCreateNegativeTimings() {
        val metrics = CaptureTelemetry(false, true)
        metrics.callbackStarted(100, 200)
        metrics.callbackStarted(90, 0)
        metrics.callbackStarted(110, 100)
        metrics.callbackFinished(-1)
        val s = metrics.snapshot()
        assertEquals(2L, s.invalidDeliveryTimestamps)
        assertEquals(1L, s.deliveryAge.samples)
        assertEquals(10L, s.deliveryAge.maxNs)
        assertEquals(1L, s.callbackInterval.samples)
        assertEquals(20L, s.callbackInterval.maxNs)
        assertEquals(0L, s.callbackDuration.samples)
    }

    @Test fun snapshotsAreImmutableAndCountersStayAccurateAcrossLongRuns() {
        val metrics = CaptureTelemetry(false, true)
        val empty = metrics.snapshot()
        repeat(100_000) { i ->
            val arrival = i * 100L + 100L
            metrics.notification(true)
            metrics.callbackStarted(arrival, arrival - 10)
            metrics.callbackFinished(5)
        }
        val s = metrics.snapshot()
        assertEquals(0L, empty.acquiredImages)
        assertEquals(100_000L, s.acquiredImages)
        assertEquals(99_999L, s.callbackInterval.samples)
        assertEquals(100.0, s.callbackInterval.meanNs, 0.0)
        assertEquals(10.0, s.deliveryAge.meanNs, 0.0)
    }
}
