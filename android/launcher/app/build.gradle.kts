// ZettaBridge launcher. Generated native/runtime inputs come only from build/launcher.
// Plain Java, UI built in code, no AndroidX.
import java.io.File
import java.util.Properties

plugins {
    id("com.android.application")
}

// The release key lives outside the repository: ~/.zettabridge/signing.properties names the
// keystore and its passwords. Without that file the release build stays unsigned, so a clone
// without the key still builds.
val signingProps = Properties()
val signingPropsFile = File(System.getProperty("user.home"), ".zettabridge/signing.properties")
if (signingPropsFile.isFile) {
    signingPropsFile.inputStream().use { stream -> signingProps.load(stream) }
}
val releaseKeystore: File? = signingProps.getProperty("storeFile")?.let { path -> File(path) }
val hasReleaseKey = releaseKeystore != null && releaseKeystore.isFile

android {
    namespace = "com.zettabridge.launcher"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.zettabridge.launcher"
        minSdk = 26
        targetSdk = 35
        versionCode = 2
        versionName = "0.1.1"
        ndk { abiFilters += "arm64-v8a" }
    }

    signingConfigs {
        if (hasReleaseKey) {
            create("release") {
                storeFile = releaseKeystore
                storePassword = signingProps.getProperty("storePassword")
                keyAlias = signingProps.getProperty("keyAlias")
                keyPassword = signingProps.getProperty("keyPassword")
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            if (hasReleaseKey) signingConfig = signingConfigs.getByName("release")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    sourceSets.getByName("main") {
        assets.srcDir(rootProject.file("../../build/launcher/assets"))
        jniLibs.srcDir(rootProject.file("../../build/launcher/jniLibs"))
    }
}

dependencies {
    // Apache-2.0. Lifts hidden API restrictions for the framework hooks in the :guest process.
    implementation("org.lsposed.hiddenapibypass:hiddenapibypass:6.1")
}
