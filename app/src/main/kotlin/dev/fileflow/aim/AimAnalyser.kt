package dev.fileflow.aim

import java.nio.ByteBuffer

/**
 * Kotlin face of the aiming analyser (C02a, feature UI-02).
 *
 * The judgement is in `core/src/framing.cpp` where the desktop suite tests it (ADR-0014). Two of its
 * defects produced *confident wrong advice* rather than errors and were caught off-device, which is
 * the whole argument for it not living here (F33).
 *
 * ⚠ [Slot] MUST match the enum in `jni_framing.cpp`. The count is checked against native at startup
 * so a mismatch fails at the first call instead of quietly shifting every field by one.
 */
enum class AimVerdict {
    NoScreenFound, Clipped, TooFar, TooDark, TooBright, Blurred, Ready, Unknown;

    companion object {
        fun from(ordinal: Int): AimVerdict =
            entries.getOrNull(ordinal) ?: Unknown
    }
}

data class Aim(
    val verdict: AimVerdict,
    val guidance: String,
    val litFraction: Double,
    val bboxX: Int, val bboxY: Int, val bboxW: Int, val bboxH: Int,
    val clippedLeft: Boolean, val clippedTop: Boolean,
    val clippedRight: Boolean, val clippedBottom: Boolean,
    val rotationDeg: Double,
    val pxPerCell: Double,
    val bboxInflation: Double,
    val midFraction: Double,
    val meanLuminance: Double,
    val threshold: Int,
    /**
     * Distance between the two luminance levels. THE focus metric.
     *
     * Mid-fraction is not one: it counts pixels between the levels, which only means "sharp" when
     * two levels exist. Bad enough focus collapses them, everything lands in one class, and
     * mid-fraction falls to zero — so the blurriest frame scores best (F40).
     */
    val levelSeparation: Double,
    /**
     * Level separation per tile across the screen, row-major, [tileCols] wide.
     *
     * Exists because the frame-wide [levelSeparation] cannot see the failure that is actually
     * blocking the link: a band of the screen outside a shallow depth of field reads as unusable
     * while the average still looks healthy (F47). The operator tilts the phone until no tile is
     * dark.
     */
    val tiles: DoubleArray,
    val tileCols: Int,
) {
    val clipped: Boolean get() = clippedLeft || clippedTop || clippedRight || clippedBottom
    val ready: Boolean get() = verdict == AimVerdict.Ready
}

/**
 * Analyses camera frames and says what to change.
 *
 * Reuses one `double[]` across calls: this runs several times a second and allocating per frame on
 * that path is the habit CONTRIBUTING bans. Not thread-safe — feed it from one thread.
 */
class AimAnalyser {

    private companion object {
        val TILE_COLS = NativeAim.tileCols()
    }

    private val slots = DoubleArray(NativeAim.slotCount())
    private val tiles = DoubleArray(NativeAim.tileCount())

    init {
        require(slots.size >= Slot.entries.size) {
            "native reports ${slots.size} slots but Kotlin expects ${Slot.entries.size} — the ABI " +
                "has drifted"
        }
    }

    /** Returns null when the frame could not be analysed at all (bad buffer, bad geometry). */
    fun analyse(y: ByteBuffer, width: Int, height: Int, rowStride: Int, cols: Int, rows: Int): Aim? {
        val guidance = NativeAim.analyse(y, width, height, rowStride, cols, rows, slots, tiles)
        val v = slots[Slot.Verdict.ordinal]
        if (v < 0.0) return null
        return Aim(
            verdict = AimVerdict.from(v.toInt()),
            guidance = guidance,
            litFraction = slots[Slot.LitFraction.ordinal],
            bboxX = slots[Slot.BboxX.ordinal].toInt(),
            bboxY = slots[Slot.BboxY.ordinal].toInt(),
            bboxW = slots[Slot.BboxW.ordinal].toInt(),
            bboxH = slots[Slot.BboxH.ordinal].toInt(),
            clippedLeft = slots[Slot.ClippedLeft.ordinal] != 0.0,
            clippedTop = slots[Slot.ClippedTop.ordinal] != 0.0,
            clippedRight = slots[Slot.ClippedRight.ordinal] != 0.0,
            clippedBottom = slots[Slot.ClippedBottom.ordinal] != 0.0,
            rotationDeg = slots[Slot.RotationDeg.ordinal],
            pxPerCell = slots[Slot.PxPerCell.ordinal],
            bboxInflation = slots[Slot.BboxInflation.ordinal],
            midFraction = slots[Slot.MidFraction.ordinal],
            meanLuminance = slots[Slot.MeanLuminance.ordinal],
            threshold = slots[Slot.Threshold.ordinal].toInt(),
            levelSeparation = slots[Slot.LevelSeparation.ordinal],
            tiles = tiles.copyOf(),
            tileCols = TILE_COLS,
        )
    }

    /** Declaration order IS the wire format. Keep in lockstep with `Slot` in `jni_framing.cpp`. */
    private enum class Slot {
        Verdict, LitFraction, BboxX, BboxY, BboxW, BboxH,
        ClippedLeft, ClippedTop, ClippedRight, ClippedBottom,
        RotationDeg, PxPerCell, BboxInflation, MidFraction, MeanLuminance, Threshold,
        LevelSeparation,
    }
}

internal object NativeAim {
    init {
        System.loadLibrary("fileflow")
    }

    @JvmStatic external fun slotCount(): Int
    @JvmStatic external fun tileCount(): Int
    @JvmStatic external fun tileCols(): Int
    @JvmStatic external fun tileUsableSeparation(): Double
    @JvmStatic external fun analyse(
        y: ByteBuffer, width: Int, height: Int, rowStride: Int, cols: Int, rows: Int,
        out: DoubleArray, tiles: DoubleArray,
    ): String
}
