package dev.fileflow

/**
 * What the sender trades: bits per cell (black and white, or eight colours), cell size (how close
 * and steady the receiver must be) and display states per second (how fast the receiver's camera
 * must be). The receiver needs to know none of it -- it detects the grid (F58), reads the
 * profile from every header (F62) and takes whatever state rate arrives.
 *
 * Eight colours carry three bits a cell where black and white carries one: the same frames, three
 * times the data. Four colours carry two, and are far easier to tell apart -- any two differ in two
 * channels -- for cameras that blur colour too much for eight (the Pixel 8, F66). Black and white
 * stays as Steady, for a camera or light that colour does not survive at all.
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
    /** Colours per cell: 8 (three bits), 4 (two bits, F66), or 0 for black and white (one). */
    val colours: Int,
) {
    Steady("Steady", 120, 260, 20.0, colours = 0),
    Fast("Fast", 120, 260, 30.0, colours = 4),
    Fastest("Fastest", 120, 260, 30.0, colours = 8);

    companion object {
        // Four colours: the setting that works on the most receivers. Eight colours ran at 265 KB/s
        // into the S26 Ultra (F63) but the Pixel 8 could not separate them (F66); Fastest stays
        // a tap away for receivers that can.
        val DEFAULT = Fast
        fun named(name: String?): Speed = entries.firstOrNull { it.name == name } ?: DEFAULT
    }
}
