package dev.fileflow

import android.content.Context
import android.util.Log
import java.io.File

/**
 * Logs a send or receive session to logcat AND to a file in the app's own storage.
 *
 * Logcat alone lost the evidence twice: installing a build clears it, and the Pixel's buffer had
 * already rolled over before the first 2.2 MB transfer could be read back, so of a two-phone test
 * only one side survived. App files survive a reinstall (`adb install -r`), and a debug build can be
 * read with `adb shell run-as dev.fileflow ls files/logs`. The newest [KEEP] sessions are kept.
 */
object SessionLog {
    private const val DIR = "logs"
    private const val KEEP = 20

    private val writers = SessionLogWriter()
    private val clock = java.text.SimpleDateFormat("HH:mm:ss.SSS", java.util.Locale.US)

    /**
     * Starts a file such as `receive-20261029-204048-123-00000042.txt`.
     * Milliseconds plus a monotonically increasing owner distinguish rapid starts; padding keeps
     * filename retention chronological. Save the returned token and pass it to [close].
     */
    @Synchronized
    fun start(context: Context, kind: String): Long = writers.start { owner ->
        runCatching {
            val dir = File(context.filesDir, DIR).apply { mkdirs() }
            dir.listFiles()?.sortedByDescending { it.name.substringAfter('-') }
                ?.drop(KEEP - 1)?.forEach { it.delete() }
            val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss-SSS", java.util.Locale.US)
                .format(java.util.Date())
            val file = File(dir, "$kind-$stamp-${owner.toString().padStart(8, '0')}.txt")
            // Never truncate an existing log, even after an unlikely clock/process collision.
            check(file.createNewFile()) { "session log already exists" }
            file.bufferedWriter()
        }.onFailure { Log.w("FileFlow.Log", "no session log: ${it.message}") }.getOrNull()
    }

    @Synchronized
    fun i(tag: String, message: String) {
        Log.i(tag, message)
        write(tag, message)
    }

    @Synchronized
    fun w(tag: String, message: String, e: Throwable? = null) {
        Log.w(tag, message, e)
        write(tag, if (e != null) "$message: $e" else message)
    }

    @Synchronized
    fun close(owner: Long): Boolean = writers.close(owner)

    private fun write(tag: String, message: String) {
        // Flushed per line: a session usually ends with the app killed or the phone pocketed, and
        // the lines nearest the end are the ones that explain it.
        writers.write("${clock.format(java.util.Date())} $tag: $message\n")
    }
}
