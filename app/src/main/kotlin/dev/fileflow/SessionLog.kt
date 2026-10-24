package dev.fileflow

import android.content.Context
import android.util.Log
import java.io.File
import java.io.Writer

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

    private var out: Writer? = null
    private val clock = java.text.SimpleDateFormat("HH:mm:ss.SSS", java.util.Locale.US)

    /** Starts a new file, "receive-20261023-214442.txt" for [kind] "receive". */
    @Synchronized
    fun start(context: Context, kind: String) {
        close()
        runCatching {
            val dir = File(context.filesDir, DIR).apply { mkdirs() }
            dir.listFiles()?.sortedByDescending { it.name.substringAfter('-') }
                ?.drop(KEEP - 1)?.forEach { it.delete() }
            val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
                .format(java.util.Date())
            out = File(dir, "$kind-$stamp.txt").bufferedWriter()
        }.onFailure { Log.w("FileFlow.Log", "no session log: ${it.message}") }
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
    fun close() {
        runCatching { out?.close() }
        out = null
    }

    private fun write(tag: String, message: String) {
        val w = out ?: return
        // Flushed per line: a session usually ends with the app killed or the phone pocketed, and
        // the lines nearest the end are the ones that explain it.
        runCatching {
            w.write("${clock.format(java.util.Date())} $tag: $message\n")
            w.flush()
        }.onFailure { close() }
    }
}
