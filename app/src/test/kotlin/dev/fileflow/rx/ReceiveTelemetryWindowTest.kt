package dev.fileflow.rx

import org.junit.Assert.*
import org.junit.Test

class ReceiveTelemetryWindowTest {
    private fun sample(session: Long = 1, switches: Long = 0, frames: Long = 60, distinct: Long = 30) =
        ReceiveTelemetryWindow.Sample(session, switches, frames, distinct, frames, 0, 0,
            listOf(frames * 2.0, frames * 3.0))

    @Test fun sessionSwitchAndSeparateGridNeverCreateAnImpossibleStateRate() {
        val first = ReceiveTelemetryWindow()
        val second = ReceiveTelemetryWindow()
        assertNull(first.observe(1_000_000_000, sample(frames = 600, distinct = 300)))
        assertNull(second.observe(2_000_000_000, sample(frames = 60, distinct = 1)))
        assertEquals(30.0, first.observe(3_000_000_000, sample(frames = 720, distinct = 360))!!.statesPerSecond, 0.0)
        assertNull(first.observe(4_000_000_000, sample(session = 2, switches = 1, frames = 780, distinct = 1)))
        assertEquals(30.0, first.observe(5_000_000_000, sample(session = 2, switches = 1, frames = 840, distinct = 31))!!.statesPerSecond, 0.0)
    }

    @Test fun staleLaneCannotReplaceTheCurrentWindow() {
        val window = ReceiveTelemetryWindow()
        window.observe(1_000_000_000, sample())
        window.observe(2_000_000_000, sample(frames = 120, distinct = 60))
        assertNull(window.observe(2_500_000_000, sample()))
        val delta = window.observe(3_000_000_000, sample(frames = 180, distinct = 90))!!
        assertEquals(30.0, delta.statesPerSecond, 0.0)
        assertEquals(listOf(2.0, 3.0), delta.stageMeansMs)
    }
}
