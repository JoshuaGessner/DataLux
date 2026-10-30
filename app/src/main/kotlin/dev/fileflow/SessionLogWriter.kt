package dev.fileflow

import java.io.Writer

/** Owns the active file writer without an Android dependency, so lifecycle races are testable. */
internal class SessionLogWriter {
    private data class Active(val owner: Long, val writer: Writer)
    private var sequence = 0L
    private var active: Active? = null

    /** Replacing a session always invalidates its old owner, including when opening fails. */
    @Synchronized fun start(open: (Long) -> Writer?): Long {
        closeCurrent()
        sequence = Math.incrementExact(sequence)
        val owner = sequence
        active = open(owner)?.let { Active(owner, it) }
        return owner
    }

    /** A retiring activity cannot close a writer created by a newer activity. */
    @Synchronized fun close(owner: Long): Boolean {
        if (active?.owner != owner) return false
        closeCurrent()
        return true
    }

    /** Writes are flushed immediately; a failed file must not break the app or future sessions. */
    @Synchronized fun write(text: String): Boolean {
        val current = active ?: return false
        return try {
            current.writer.write(text)
            current.writer.flush()
            true
        } catch (_: Exception) {
            close(current.owner)
            false
        }
    }

    private fun closeCurrent() {
        val current = active
        active = null
        runCatching { current?.writer?.close() }
    }
}
