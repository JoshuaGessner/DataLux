package dev.fileflow

/**
 * What the sender trades: bits per cell (black and white, or eight colours), cell size (how close
 * and steady the receiver must be) and display states per second (how fast the receiver's camera
 * must be). The receiver needs to know none of it -- it detects the grid (F58), reads the
 * profile from every header (F62) and takes whatever state rate arrives.
 *
 * Colour carries three bits a cell where black and white carries one: the same frames, three times
 * the data. Black and white stays as Steady, the setting for a camera or a light that colour does
 * not survive.
 *
 * The rates come from the unsynchronised-capture sweep (F60, simulated `[HYP]`): with a 30 fps
 * camera goodput peaks at a display rate of about 20 and FALLS beyond it, as captures tear; with a
 * 60 fps camera it is still rising at 30-40. Steady is therefore the setting that suits every
 * camera, and Fast/Fastest assume the receiver captures at 60, which both reference phones do.
 */
enum class Speed(
    val label: String,
    val cols: Int,
    val rows: Int,
    val statesPerSecond: Double,
    val colour: Boolean,
) {
    Steady("Steady", 120, 260, 20.0, colour = false),
    Fast("Fast", 120, 260, 20.0, colour = true),
    Fastest("Fastest", 120, 260, 30.0, colour = true);

    companion object {
        // Colour at 30/s: 263 KB/s Pixel 8 -> S26 Ultra, against 178 at 20/s (F63).
        val DEFAULT = Fastest
        fun named(name: String?): Speed = entries.firstOrNull { it.name == name } ?: DEFAULT
    }
}
