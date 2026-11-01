package dev.fileflow.capture

/** Constant-space timing counters. Mutated on the camera thread; publish immutable snapshots. */
class CaptureTelemetry(
    private val latestFrameOnly: Boolean,
    private val timestampsAreRealtime: Boolean,
) {
    data class Timing(val samples: Long = 0, val meanNs: Double = 0.0, val maxNs: Long = 0)

    data class Summary(
        val latestFrameOnly: Boolean = false,
        val timestampsAreRealtime: Boolean = false,
        val imageNotifications: Long = 0,
        val acquiredImages: Long = 0,
        val emptyNotifications: Long = 0,
        val callbackCount: Long = 0,
        val callbackInterval: Timing = Timing(),
        val deliveryAge: Timing = Timing(),
        val callbackDuration: Timing = Timing(),
        val invalidDeliveryTimestamps: Long = 0,
        val captureFailures: Long = 0,
    ) {
        /** acquireLatestImage discards internally; ImageReader does not expose an exact count. */
        val supersededImages: Long? get() = null
    }

    private class Counter {
        var samples = 0L
        var mean = 0.0
        var max = 0L
        fun add(ns: Long) {
            if (ns < 0) return
            samples++
            mean += (ns - mean) / samples
            max = maxOf(max, ns)
        }
        fun snapshot() = Timing(samples, mean, max)
    }

    private var notifications = 0L
    private var acquired = 0L
    private var empty = 0L
    private var callbacks = 0L
    private var invalidTimestamps = 0L
    private var failures = 0L
    private var lastCallbackNs: Long? = null
    private val intervals = Counter()
    private val ages = Counter()
    private val durations = Counter()

    @Synchronized fun notification(acquiredImage: Boolean) {
        notifications++
        if (acquiredImage) acquired++ else empty++
    }

    @Synchronized fun callbackStarted(arrivalNs: Long, captureNs: Long) {
        lastCallbackNs?.let { if (arrivalNs >= it) intervals.add(arrivalNs - it) }
        lastCallbackNs = arrivalNs
        callbacks++
        if (timestampsAreRealtime) {
            if (captureNs > 0L && captureNs <= arrivalNs) ages.add(arrivalNs - captureNs)
            else invalidTimestamps++
        }
    }

    @Synchronized fun callbackFinished(durationNs: Long) = durations.add(durationNs)

    @Synchronized fun captureFailed() { failures++ }

    @Synchronized fun snapshot() = Summary(
        latestFrameOnly, timestampsAreRealtime, notifications, acquired, empty, callbacks,
        intervals.snapshot(), ages.snapshot(), durations.snapshot(), invalidTimestamps, failures,
    )
}
