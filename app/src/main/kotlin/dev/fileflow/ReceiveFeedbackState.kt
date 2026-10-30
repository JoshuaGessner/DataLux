package dev.fileflow

/** UI-clock state: readable headers and CRC-valid manifests are not useful file progress. */
internal class ReceiveFeedbackState(private val productiveWindowMs: Long = 2500L) {
    data class Sample(
        val sessionId: Long = 0,
        val senderSwitches: Long = 0,
        val frames: Long = 0,
        val usefulSymbols: Long = 0,
        val progress: Double = 0.0,
        val patternFound: Boolean = false,
        val cols: Int = 0,
        val rows: Int = 0,
    )

    data class Feedback(
        val accepted: Boolean,
        val productive: Boolean,
        val patternFound: Boolean,
        val progress: Double,
        val stalledForMs: Long,
        val epochChanged: Boolean = false,
    )

    private var previous: Sample? = null
    private var epochStartedMs = 0L
    private var lastProgressMs: Long? = null
    private var progress = 0.0
    private var usefulSymbols = 0L

    fun update(sample: Sample, nowMs: Long): Feedback {
        val old = previous
        val sameCandidate = old != null && sample.cols == old.cols && sample.rows == old.rows
        val stale = old != null && sameCandidate && (sample.senderSwitches < old.senderSwitches ||
            (sample.senderSwitches == old.senderSwitches && old.sessionId != 0L &&
                sample.sessionId != old.sessionId) ||
            (sample.senderSwitches == old.senderSwitches && sample.sessionId == old.sessionId &&
                sample.frames < old.frames))
        if (stale) return feedback(false, old!!.patternFound, nowMs)

        val newEpoch = old == null || !sameCandidate || sample.senderSwitches != old.senderSwitches ||
            sample.sessionId != old.sessionId
        if (newEpoch) {
            epochStartedMs = nowMs
            lastProgressMs = null
            progress = 0.0
            usefulSymbols = 0L
        }
        val nextProgress = sample.progress.takeIf { it.isFinite() }?.coerceIn(0.0, 1.0) ?: progress
        if (sample.usefulSymbols > usefulSymbols || nextProgress > progress) lastProgressMs = nowMs
        usefulSymbols = maxOf(usefulSymbols, sample.usefulSymbols)
        progress = maxOf(progress, nextProgress)
        previous = sample
        return feedback(true, sample.patternFound, nowMs).copy(epochChanged = newEpoch)
    }

    private fun feedback(accepted: Boolean, pattern: Boolean, nowMs: Long): Feedback {
        val last = lastProgressMs
        return Feedback(accepted, last != null && nowMs - last < productiveWindowMs,
            pattern, progress, (nowMs - (last ?: epochStartedMs)).coerceAtLeast(0))
    }
}

/** A stopped/replaced receive cannot publish a late camera error or stale decoder callback. */
internal class LiveCaptureSession {
    private var generation = 0L
    private var active = false

    @Synchronized fun start(): Long {
        generation++
        active = true
        return generation
    }

    @Synchronized fun accepts(token: Long): Boolean = active && token == generation

    /** True exactly once for the active generation. Explicit stop makes cancellation quiet. */
    @Synchronized fun stop(token: Long): Boolean {
        if (!accepts(token)) return false
        active = false
        return true
    }
}
