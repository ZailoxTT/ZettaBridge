// T6: runs the zbrun guest test suite through libzbridge.so inside an app process next to ART.
// Plain Java, no AndroidX. jniLibs/assets/java come from tools/make_t6_bundle.sh.
import java.io.File
import java.util.Properties

plugins {
    id("com.android.application")
}

val signingProps = Properties()
val signingPropsFile = File(System.getProperty("user.home"), ".zettabridge/signing.properties")
if (signingPropsFile.isFile) {
    signingPropsFile.inputStream().use { stream -> signingProps.load(stream) }
}
val releaseKeystore: File? = signingProps.getProperty("storeFile")?.let { path -> File(path) }
val hasReleaseKey = releaseKeystore != null && releaseKeystore.isFile

android {
    namespace = "com.zettabridge.t6"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.zettabridge.t6"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"
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
        assets.srcDir(rootProject.file("../../../build/t6/assets"))
        java.srcDir(rootProject.file("../../../build/t6/java"))
        jniLibs.srcDir(rootProject.file("../../../build/t6/jniLibs"))
    }
}
