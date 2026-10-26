package dev.fileflow

/** UI pacing only; the account server is authoritative about email requests and limits. */
object VerificationCooldown {
    const val REQUEST_MILLIS = 60_000L
    private const val MAX_MILLIS = 3_600_000L

    fun secondsRemaining(resendAt: Long, now: Long = System.currentTimeMillis()): Long =
        ((resendAt.coerceAtMost(now + MAX_MILLIS) - now).coerceAtLeast(0) + 999) / 1000

    fun serverDeadline(value: Long, now: Long = System.currentTimeMillis()): Long =
        if (value > now) value.coerceAtMost(now + MAX_MILLIS) else 0

    fun retryDeadline(header: String?, now: Long = System.currentTimeMillis()): Long =
        now + ((header?.toLongOrNull() ?: 60).coerceIn(1, 3600) * 1000)
}
