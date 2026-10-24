package dev.fileflow

/**
 * What the sender trades: cell size (how close and steady the receiver must be) against display
 * states per second (how fast the receiver's camera must be). The receiver needs to know neither
 * -- it detects the grid (F58) and reads whatever state rate arrives.
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
) {
    Steady("Steady", 120, 260, 20.0),
    Fast("Fast", 120, 260, 30.0),
    Fastest("Fastest", 180, 390, 30.0);

    companion object {
        val DEFAULT = Steady
        fun named(name: String?): Speed = entries.firstOrNull { it.name == name } ?: DEFAULT
    }
}
