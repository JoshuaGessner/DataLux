// Root build file. Plugin versions are pinned here and applied in :app.
//
// AGP 9 compiles Kotlin itself ("built-in Kotlin"), so there is no separate Kotlin plugin.
plugins {
    id("com.android.application") version "9.4.1" apply false
}
