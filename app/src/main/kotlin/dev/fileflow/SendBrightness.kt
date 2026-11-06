package dev.fileflow

/** A stable display brightness for one optical transfer, selectable before sending. */
enum class SendBrightness(val label: String, val level: Float) {
    Low("Low", 0.4f),
    Medium("Medium", 0.65f),
    Full("Full", 1f);

    companion object {
        const val PREFERENCE = "send_brightness"
        fun named(name: String?): SendBrightness = entries.firstOrNull { it.name == name } ?: Full
    }
}
