plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.fileflow"
    compileSdk = 35

    defaultConfig {
        applicationId = "dev.fileflow"
        // ADR-0013 / ADR-0004: FrameTimeline and AChoreographer_postVsyncCallback are API 33.
        minSdk = 33
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0-phase2"

        ndk {
            // arm64 only (ADR-0013). HWASan is ARM64-only anyway, and the emulator is useless
            // to us -- it has neither a real camera nor a real display pipeline.
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                // ADR-0013: exactly one .so, default c++_static. Adding a second shared library
                // requires switching the whole project to c++_shared in the same change.
                arguments += listOf("-DANDROID_STL=c++_static")
                cppFlags += "-std=c++20"
            }
        }
    }

    externalNativeBuild {
        cmake {
            // NOT the root CMakeLists: that one pulls GoogleTest and builds the simulator,
            // neither of which belongs in an APK.
            path = file("../platform/android/CMakeLists.txt")
            // Pinned deliberately. AGP silently defaults to 3.10.2 when unset `[FACT]`, which
            // predates CMakePresets and would not build this project.
            version = "3.22.1"
        }
    }

    ndkVersion = "29.0.14206865"  // ADR-0013: NDK r29

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
        debug {
            isJniDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs {
            // Keep the .so uncompressed so it can be mapped directly rather than extracted.
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.3")
    // Material 3: the components (cards, segmented buttons, dialogs, progress) and the dynamic
    // colour that make the app look like the rest of the phone rather than a debug tool.
    implementation("com.google.android.material:material:1.12.0")
}
