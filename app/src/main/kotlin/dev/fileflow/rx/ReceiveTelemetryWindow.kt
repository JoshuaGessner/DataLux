package dev.fileflow.rx

/** Per-grid, per-transfer deltas. Counters from another candidate/epoch never inflate state rate. */
internal class ReceiveTelemetryWindow {
    data class Sample(
        val session: Long, val switches: Long, val frames: Long, val distinct: Long,
        val headers: Long, val fecFailures: Long, val crcFailures: Long,
        val stagesMs: List<Double>,
    )
    data class Delta(
        val seconds: Double, val frames: Long, val distinct: Long,
        val headers: Long, val fecFailures: Long, val crcFailures: Long,
        val stageMeansMs: List<Double>,
    ) {
        val statesPerSecond: Double get() = distinct / seconds
    }
    private var previous: Pair<Long, Sample>? = null

    @Synchronized
    fun observe(nowNs: Long, sample: Sample): Delta? {
        val old = previous
        if (old != null && (nowNs <= old.first || sample.frames < old.second.frames ||
                sample.switches < old.second.switches)) return null
        previous = nowNs to sample
        if (old == null || sample.session != old.second.session || sample.switches != old.second.switches) return null
        val before = old.second
        val frames = sample.frames - before.frames
        if (frames <= 0 || sample.distinct < before.distinct || sample.headers < before.headers ||
            sample.fecFailures < before.fecFailures || sample.crcFailures < before.crcFailures ||
            sample.stagesMs.size != before.stagesMs.size) return null
        val stages = sample.stagesMs.zip(before.stagesMs).map { (after, start) -> (after - start) / frames }
        if (stages.any { !it.isFinite() || it < 0.0 }) return null
        return Delta((nowNs - old.first) / 1e9, frames, sample.distinct - before.distinct,
            sample.headers - before.headers, sample.fecFailures - before.fecFailures,
            sample.crcFailures - before.crcFailures, stages)
    }
}
