import java.net.URI
import java.security.KeyStore
import java.security.KeyFactory
import java.security.PrivateKey
import java.security.MessageDigest
import java.security.cert.X509Certificate
import java.security.interfaces.ECPublicKey
import java.security.interfaces.RSAPublicKey
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
val storeBuild = providers.gradleProperty("store").orElse("false").get() == "true"
val releaseVersionCode = providers.gradleProperty("fileflowVersionCode").orElse("3").get().toIntOrNull()
require(releaseVersionCode != null && releaseVersionCode in 1..2_100_000_000) { "fileflowVersionCode must be a positive Play-compatible integer" }
val releaseVersionName = providers.gradleProperty("fileflowVersionName").orElse("0.3.0").get()
require(releaseVersionName.matches(Regex("[0-9]+\\.[0-9]+\\.[0-9]+(?:-[0-9A-Za-z.-]+)?"))) { "fileflowVersionName must be a release version" }

// Upload signing and offline-Pro signing are separate identities. Only the public Pro key
// enters the app. Upload passwords come from the environment, never command-line properties.
val uploadStorePath = providers.environmentVariable("FILEFLOW_UPLOAD_KEYSTORE").orNull
val uploadStorePassword = providers.environmentVariable("FILEFLOW_UPLOAD_STORE_PASSWORD").orNull
val uploadKeyAlias = providers.environmentVariable("FILEFLOW_UPLOAD_KEY_ALIAS").orNull
val uploadKeyPassword = providers.environmentVariable("FILEFLOW_UPLOAD_KEY_PASSWORD").orNull
val uploadConfigured = listOf(uploadStorePath, uploadStorePassword, uploadKeyAlias, uploadKeyPassword).all { !it.isNullOrBlank() }

val validatePlayRelease = tasks.register("validatePlayRelease") {
    group = "verification"
    description = "Require the store profile, offline-Pro trust, and a private stable upload identity. Does not publish."
    doLast {
        require(storeBuild) { "Play bundles require -Pstore=true; community/source builds remain available as APKs." }
        require(licensePublicKey.isNotEmpty()) { "Play releases require a configured FileFlow license public key." }
        require(uploadConfigured) { "Play releases require all four FILEFLOW_UPLOAD_* environment settings. Keep the keystore outside the checkout." }
        val keyFile = file(uploadStorePath!!).canonicalFile
        require(keyFile.isFile && !keyFile.toPath().startsWith(rootDir.canonicalFile.toPath())) { "The upload keystore must be an existing private file outside the checkout." }
        val certificate = try {
            val keyStore = KeyStore.getInstance(keyFile, uploadStorePassword!!.toCharArray())
            require(keyStore.isKeyEntry(uploadKeyAlias!!))
            require(keyStore.getKey(uploadKeyAlias, uploadKeyPassword!!.toCharArray()) is PrivateKey)
            (keyStore.getCertificate(uploadKeyAlias) as X509Certificate).also {
                it.checkValidity()
                require(!it.subjectX500Principal.name.contains("CN=Android Debug", ignoreCase = true))
                require(when (val publicKey = it.publicKey) {
                    is RSAPublicKey -> publicKey.modulus.bitLength() >= 2048
                    is ECPublicKey -> publicKey.params.curve.field.fieldSize >= 256
                    else -> false
                })
            }
        } catch (_: Exception) {
            throw GradleException("Upload signing could not be verified. Check the private keystore, alias, passwords and certificate validity. Debug keys are not accepted.")
        }
        // Public certificate only: included beside the review artifact so the owner can compare
        // it with Play Console before first upload. No private-key bytes leave the keystore.
        val publicCertificate = layout.buildDirectory.file("play-review/upload-certificate.der").get().asFile
        publicCertificate.parentFile.mkdirs()
        publicCertificate.writeBytes(certificate.encoded)
        publicCertificate.parentFile.resolve("license-public-key.der").writeBytes(Base64.getDecoder().decode(licensePublicKey))
        val fingerprint = MessageDigest.getInstance("SHA-256").digest(certificate.encoded).joinToString(":") { "%02X".format(it) }
        logger.lifecycle("Upload certificate SHA-256: $fingerprint")
    }
}

android {
    namespace = "dev.fileflow"
    compileSdk = 37

    defaultConfig {
        applicationId = "dev.fileflow"
        // ADR-0013 / ADR-0004: FrameTimeline and AChoreographer_postVsyncCallback are API 33.
        minSdk = 33
        targetSdk = 37
        versionCode = releaseVersionCode
        versionName = releaseVersionName

        // A store build (`-Pstore=true`) locks paid features behind what was bought
        // (Entitlements.kt). Every other build -- yours, from this source -- has them all.
        buildConfigField("boolean", "STORE", storeBuild.toString())
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

    signingConfigs {
        if (uploadConfigured) create("playUpload") {
            storeFile = file(uploadStorePath!!)
            storePassword = uploadStorePassword
            keyAlias = uploadKeyAlias
            keyPassword = uploadKeyPassword
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (storeBuild && uploadConfigured) signingConfig = signingConfigs.getByName("playUpload")
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

// Every release AAB is intended for Play and must pass the guard, including direct Gradle
// invocations. Source/debug APKs and ordinary community release APKs need no paid identity.
tasks.matching { it.name == "bundleRelease" || it.name == "signReleaseBundle" ||
    (storeBuild && it.name in listOf("assembleRelease", "packageRelease")) }.configureEach {
    dependsOn(validatePlayRelease)
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
