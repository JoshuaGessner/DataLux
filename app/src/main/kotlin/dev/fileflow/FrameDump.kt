package dev.fileflow

import android.content.Context
import dev.fileflow.capture.CameraRecorder
import java.io.File
import java.nio.ByteBuffer
import java.util.concurrent.Executors

/**
 * Saves raw camera frames -- luma and both chroma planes, packed -- to `files/frames/`, for
 * working out offline why a phone cannot read what another can.
 *
 * A log says that colour failed; only the frame says how: which colours were confused, next to
 * which neighbours, where on the screen. The desktop tool `ffframe` regenerates exactly what the
 * sender drew for that frame and compares the two cell by cell. Each frame is ~14 MB at 4032x2268,
 * so only the newest [KEEP] are kept -- and only in a debuggable build -- which reads them with
 * `adb shell run-as dev.fileflow ls files/frames`.
 *
 * Format: `<name>.yuv` is Y (w*h) then U (w/2*h/2) then V, no padding; `<name>.txt` the context.
 */
object FrameDump {
    private const val DIR = "frames"
    private const val KEEP = 12
    private val writer = Executors.newSingleThreadExecutor { r -> Thread(r, "ff-frame-dump") }

    /**
     * Copies the frame now (the buffers go back to the camera when the callback returns) and
     * writes it in the background. Returns false without chroma: a luma-only frame cannot answer
     * a colour question.
     */
    fun save(
        context: Context, tag: String, y: ByteBuffer, w: Int, h: Int, stride: Int,
        chroma: CameraRecorder.Chroma?, info: String,
    ): Boolean {
        if (chroma == null || w <= 0 || h <= 0) return false
        // Development builds only. Each frame is a raw picture of what was on the sender's screen
        // and around it, kept in the app's storage -- a diagnostic, not something a person who
        // installs FileFlow should carry without asking for it.
        if (context.applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE == 0) {
            return false
        }
        val cw = w / 2
        val ch = h / 2
        val out = ByteArray(w * h + 2 * cw * ch)
        val src = y.duplicate()
        for (row in 0 until h) {
            src.position(row * stride)
            src.get(out, row * w, w)
        }
        var o = w * h
        for (plane in listOf(chroma.u, chroma.v)) {
            for (row in 0 until ch) {
                val base = row * chroma.rowStride
                for (x in 0 until cw) out[o++] = plane.get(base + x * chroma.pixelStride)
            }
        }
        val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss-SSS", java.util.Locale.US)
            .format(java.util.Date())
        val dir = File(context.filesDir, DIR)
        writer.execute {
            runCatching {
                dir.mkdirs()
                File(dir, "frame-$stamp-$tag.yuv").writeBytes(out)
                File(dir, "frame-$stamp-$tag.txt").writeText("width $w\nheight $h\n$info\n")
                dir.listFiles { f -> f.name.endsWith(".yuv") }
                    ?.sortedByDescending { it.name }?.drop(KEEP)
                    ?.forEach { stale ->
                        stale.delete()
                        File(dir, stale.name.removeSuffix(".yuv") + ".txt").delete()
                    }
                SessionLog.i("FileFlow.Live", "saved frame $stamp-$tag for diagnosis")
            }
        }
        return true
    }
}
