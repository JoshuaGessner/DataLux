import java.net.URI
import java.security.KeyFactory
import java.security.interfaces.ECPublicKey
import java.security.spec.X509EncodedKeySpec
import java.util.Base64

plugins {
    id("com.android.application")
}

val licensePublicKey = providers.gradleProperty("fileflowLicensePublicKey").orElse("").get()
require(licensePublicKey.matches(Regex("[A-Za-z0-9+/=]*"))) { "fileflowLicensePublicKey must be base64 SPKI DER" }
require(licensePublicKey.isEmpty() || runCatching {
    val key = KeyFactory.getInstance("EC").generatePublic(X509EncodedKeySpec(Base64.getDecoder().decode(licensePublicKey))) as ECPublicKey
    require(key.params.curve.field.fieldSize == 256 && key.params.order.bitLength() == 256)
}.isSuccess) { "fileflowLicensePublicKey must contain a valid P-256 SPKI public key" }
val accountOrigin = providers.gradleProperty("fileflowAccountOrigin")
    .orElse("https://fileflow.anomalousinteractive.com").get()
val accountUri = URI(accountOrigin)
require(accountUri.scheme == "https" && accountUri.host != null && accountUri.userInfo == null &&
    accountUri.rawQuery == null && accountUri.rawFragment == null && accountUri.path.isNullOrEmpty()) {
    "fileflowAccountOrigin must be an HTTPS origin without a path"
}

android {
    namespace = "dev.fileflow"
    compileSdk = 37

    defaultConfig {
        applicationId = "dev.fileflow"
        // ADR-0013 / ADR-0004: FrameTimeline and AChoreographer_postVsyncCallback are API 33.
        minSdk = 33
        targetSdk = 37
        versionCode = 2
        versionName = "0.2.0"

        // A store build (`-Pstore=true`) locks paid features behind what was bought
        // (Entitlements.kt). Every other build -- yours, from this source -- has them all.
        buildConfigField("boolean", "STORE", (findProperty("store") == "true").toString())
        buildConfigField("String", "LICENSE_PUBLIC_KEY", "\"$licensePublicKey\"")
        buildConfigField("String", "ACCOUNT_ORIGIN", "\"$accountOrigin\"")

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

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            // Keep the .so uncompressed so it can be mapped directly rather than extracted.
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation("com.android.billingclient:billing:9.1.0")
    implementation("androidx.core:core-ktx:1.19.1")
    implementation("androidx.appcompat:appcompat:1.8.0")
    implementation("androidx.activity:activity-ktx:1.13.0")
    // Material 3: the components (cards, segmented buttons, dialogs, progress) and the dynamic
    // colour that make the app look like the rest of the phone rather than a debug tool.
    implementation("com.google.android.material:material:1.14.0")
    // QR codes for pairing in encrypted mode: drawing this phone's key, reading the other's.
    // ZXing's core is pure Java (Apache-2.0) and needs no Play Services.
    implementation("com.google.zxing:core:3.5.4")

    testImplementation("junit:junit:4.13.2")
}
