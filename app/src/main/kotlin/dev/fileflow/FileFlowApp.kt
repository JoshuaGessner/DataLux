package dev.fileflow

import android.app.Application
import com.google.android.material.color.DynamicColors

/** Takes the phone's own accent colours (Material You) where it has them. */
class FileFlowApp : Application() {
    override fun onCreate() {
        super.onCreate()
        DynamicColors.applyToActivitiesIfAvailable(this)
    }
}
