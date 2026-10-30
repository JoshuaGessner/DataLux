package dev.fileflow

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ReceiveFeedbackStateTest {
    @Test fun geometryHeadersAndRepeatedManifestsNeverClaimDataFlow() {
        val state = ReceiveFeedbackState()
        assertFalse(state.update(ReceiveFeedbackState.Sample(frames = 1), 10).productive)
        repeat(20) { frame ->
            val f = state.update(ReceiveFeedbackState.Sample(frames = frame + 2L,
                patternFound = true), 100L + frame * 500)
            assertTrue(f.patternFound)
            assertFalse(f.productive)
            assertEquals(0.0, f.progress, 0.0)
        }
    }

    @Test fun usefulRankOrByteProgressStartsFlowAndStallsRetainProgress() {
        val state = ReceiveFeedbackState()
        val first = ReceiveFeedbackState.Sample(sessionId = 7, frames = 1,
            usefulSymbols = 1, progress = 0.1, patternFound = true)
        assertTrue(state.update(first, 100).productive)
        assertTrue(state.update(first.copy(frames = 2, usefulSymbols = 2), 2000).productive)
        assertFalse(state.update(first.copy(frames = 3, usefulSymbols = 2), 4500).productive)
        val resumed = state.update(first.copy(frames = 4, usefulSymbols = 2, progress = 0.2), 5000)
        assertTrue(resumed.productive)
        val paused = state.update(first.copy(frames = 5, usefulSymbols = 2, progress = 0.15,
            patternFound = false), 8000)
        assertFalse(paused.productive)
        assertEquals(0.2, paused.progress, 0.0)
    }

    @Test fun transferResetClearsFlowAndProgressAndRejectsOldEpoch() {
        val state = ReceiveFeedbackState()
        val first = ReceiveFeedbackState.Sample(sessionId = 7, frames = 20,
            usefulSymbols = 10, progress = 0.5)
        state.update(first, 100)
        val reset = state.update(ReceiveFeedbackState.Sample(senderSwitches = 1), 200)
        assertFalse(reset.productive)
        assertEquals(0.0, reset.progress, 0.0)
        assertFalse(state.update(first.copy(frames = 21), 300).accepted)
        val manifest = state.update(ReceiveFeedbackState.Sample(sessionId = 8, senderSwitches = 1,
            patternFound = true), 400)
        assertFalse(manifest.productive)
        assertEquals(0.0, manifest.progress, 0.0)
    }

    @Test fun staleCallbacksCannotReviveExpiredFlow() {
        val state = ReceiveFeedbackState()
        val latest = ReceiveFeedbackState.Sample(frames = 10, usefulSymbols = 3, progress = 0.2)
        state.update(latest, 100)
        val stale = state.update(latest.copy(frames = 9, usefulSymbols = 4, progress = 0.3), 3000)
        assertFalse(stale.accepted)
        assertFalse(stale.productive)
        assertEquals(0.2, stale.progress, 0.0)
    }

    @Test fun gridCandidatesHaveIndependentFrameAndSenderCounters() {
        val state = ReceiveFeedbackState()
        state.update(ReceiveFeedbackState.Sample(senderSwitches = 3, frames = 100,
            cols = 120, rows = 260), 100)
        val selected = state.update(ReceiveFeedbackState.Sample(sessionId = 8, frames = 1,
            usefulSymbols = 1, progress = 0.1, cols = 144, rows = 240), 200)
        assertTrue(selected.accepted)
        assertTrue(selected.productive)
        assertEquals(0.1, selected.progress, 0.0)
        val oldSession = state.update(ReceiveFeedbackState.Sample(frames = 2,
            cols = 144, rows = 240), 300)
        assertFalse(oldSession.accepted)
    }

    @Test fun cancellationAndRestartRejectLateFailureAndStopOnlyOnce() {
        val session = LiveCaptureSession()
        val first = session.start()
        assertTrue(session.accepts(first))
        assertTrue(session.stop(first))
        assertFalse(session.stop(first))
        assertFalse(session.accepts(first))
        val retry = session.start()
        assertFalse(session.accepts(first))
        assertFalse(session.stop(first))
        assertTrue(session.accepts(retry))
        assertTrue(session.stop(retry))
    }
}
